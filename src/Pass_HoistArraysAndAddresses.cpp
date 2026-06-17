#include "Pass_HoistArraysAndAddresses.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <format>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace pancake::hoist_arrays_and_addresses;

static size_t hoist_arr_counter = 0;
static size_t hoist_ptr_counter = 0;

// generate a unique global name for a hoisted array
static auto arrayPrefix(const std::string &func, const std::string &var)
    -> std::string {
  return std::format("__c2pnk_arr_{}_{}_{}", func, var, hoist_arr_counter++);
}

// generate a unique global name for a hoisted address-taken variable
static auto ptrPrefix(const std::string &func, const std::string &var)
    -> std::string {
  return std::format("__c2pnk_ptr_{}_{}_{}", func, var, hoist_ptr_counter++);
}

// Build a global declaration string for a VarDecl
static auto buildGlobalDecl(const clang::VarDecl *VD,
                            const std::string &newName, clang::ASTContext &Ctx,
                            bool preserveInit) -> std::string {
  const auto& pp(Ctx.getLangOpts());
  const auto qt = VD->getType();
  const auto sc = VD->getStorageClass();
  std::string decl;
  llvm::raw_string_ostream os(decl);
  os << ((sc == clang::SC_Extern) ? "extern " : "static ");

  qt.print(os, pp, newName);

  if (preserveInit && VD->hasInit()) {
    std::string init_str;
    llvm::raw_string_ostream init_os(init_str);
    VD->getInit()->printPretty(init_os, nullptr, pp);
    os << " = " << init_str;
  }
  os << ";";
  return os.str();
}

// Nested array init helper
//
// Walks an InitListExpr recursively, tracking the subscript path (e.g.
// "[0][2]") and emitting one flat assignment statement per scalar leaf:
//
//   float mat[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
//   ->
//   __hoist_arr_matrix_mat_0[0][0] = 1;
//   __hoist_arr_matrix_mat_0[0][1] = 0;
//   ...
static void emitNestedArrayInits(const clang::Expr *expr,
                                 const std::string &baseName,
                                 const std::string &indexPath,
                                 const clang::PrintingPolicy &pp,
                                 std::string &out) {
  const auto *stripped = expr->IgnoreParenCasts();

  if (const auto *ile = dyn_cast<clang::InitListExpr>(stripped)) {
    // Use the syntactic form if available
    // the semantic form inserts implicit zero-fills that we do not want to emit
    // explicitly
    const auto *to_walk =
        ile->isSyntacticForm() ? ile : ile->getSyntacticForm();
    if (to_walk == nullptr)
      to_walk = ile;

    for (unsigned i = 0; i < to_walk->getNumInits(); ++i) {
      emitNestedArrayInits(to_walk->getInit(i), baseName,
                           std::format("{}[{}]", indexPath, i), pp, out);
    }
  } else {
    // we are at the leaf scalar expression so emit the assignment statement
    std::string val;
    llvm::raw_string_ostream os(val);
    expr->printPretty(os, nullptr, pp);
    out += std::format("{}{} = {};\n", baseName, indexPath, val);
  }
}

auto AddressTakenFinder::VisitUnaryOperator(clang::UnaryOperator *UO) -> bool {
  if (UO->getOpcode() == clang::UO_AddrOf) {
    auto *sub = UO->getSubExpr()->IgnoreParenCasts();
    if (auto *dr = dyn_cast<clang::DeclRefExpr>(sub))
      if (auto *vd = dyn_cast<clang::VarDecl>(dr->getDecl()))
        addressTaken.insert(vd);
  }
  return true;
}

auto CollectVisitor::VisitFunctionDecl(clang::FunctionDecl *FD) -> bool {
  if (!FD->hasBody() || !FD->isThisDeclarationADefinition())
    return true;

  AddressTakenFinder atf;
  atf.TraverseStmt(FD->getBody());
  walkStmt(FD->getBody(), FD, atf.addressTaken);
  return true;
}
void CollectVisitor::walkStmt(clang::Stmt *S, clang::FunctionDecl *FD,
                              std::set<const clang::VarDecl *> &addrTaken) {
  if (S == nullptr)
    return;

  if (auto *ds = dyn_cast<clang::DeclStmt>(S)) {
    for (clang::Decl *d : ds->decls()) {
      auto *vd = dyn_cast<clang::VarDecl>(d);
      if (vd == nullptr)
        continue;

      const auto sc = vd->getStorageClass();
      const auto qt = vd->getType();
      const auto is_array = qt->isArrayType();
      const auto addr_taken_var = addrTaken.contains(vd);

      if (sc == clang::SC_Extern) {
        info.hoistMap[vd] = {.newName = vd->getName().str(),
                             .treatment = DeclTreatment::ExternRedecl,
                             .func = FD};
        continue;
      }

      if (sc == clang::SC_Static && (is_array || addr_taken_var)) {
        const auto new_name =
            is_array ? arrayPrefix(FD->getName().str(), vd->getName().str())
                     : ptrPrefix(FD->getName().str(), vd->getName().str());
        info.hoistMap[vd] = {.newName = new_name,
                             .treatment = DeclTreatment::StaticGlobal,
                             .func = FD};
        continue;
      }

      if (vd->hasLocalStorage() && (is_array || addr_taken_var)) {
        const auto new_name =
            is_array ? arrayPrefix(FD->getName().str(), vd->getName().str())
                     : ptrPrefix(FD->getName().str(), vd->getName().str());
        info.hoistMap[vd] = {.newName = new_name,
                             .treatment = DeclTreatment::NewGlobal,
                             .func = FD};
      }
    }
  }

  for (auto *child : S->children())
    walkStmt(child, FD, addrTaken);
}

auto CollectReplacementsVisitor::VisitDeclStmt(clang::DeclStmt *DS) -> bool {
  struct Partition {
    clang::VarDecl *VD{};
    VarHoistEntry *entry{};
  };
  std::vector<Partition> parts;
  bool any_hoisted = false;

  for (clang::Decl *d : DS->decls()) {
    auto *vd = dyn_cast<clang::VarDecl>(d);
    if (vd == nullptr)
      continue;
    auto it = info.hoistMap.find(vd);
    if (it != info.hoistMap.end()) {
      parts.emplace_back(vd, &it->second);
      any_hoisted = true;
    } else {
      parts.emplace_back(vd, nullptr);
    }
  }

  if (!any_hoisted)
    return true;

  const clang::PrintingPolicy pp(Ctx.getLangOpts());
  std::string replacement;

  for (auto &p : parts) {
    auto *vd = p.VD;

    if (p.entry == nullptr) {
      // wasn't hoisted
      std::string decl_str;
      llvm::raw_string_ostream os(decl_str);
      vd->print(os, pp);
      replacement += std::format("{};\n", decl_str);
      continue;
    }

    const VarHoistEntry &entry = *p.entry;

    switch (entry.treatment) {

    case DeclTreatment::ExternRedecl:
      // Remove the local re-declaration; the external symbol already exists.
      replacement += std::format(
          "/* c2pancake: extern decl {} was moved to global scope */\n",
          vd->getName().str());
      break;

    case DeclTreatment::StaticGlobal: {
      // The global decl (with constant init if applicable) was already
      // emitted before the function.  Here we only need to emit a runtime
      // re-initialisation assignment if the initialiser is not a constant.
      bool emitted = false;
      if (vd->hasInit()) {
        clang::Expr::EvalResult result;
        const bool is_const = vd->getInit()->EvaluateAsRValue(result, Ctx);
        if (!is_const) {
          std::string init_str;
          llvm::raw_string_ostream os(init_str);
          vd->getInit()->printPretty(os, nullptr, pp);
          replacement += std::format("{} = {};\n", entry.newName, init_str);
          emitted = true;
        }
      }
      if (!emitted)
        replacement += std::format(
            "/* c2pancake: static decl {} was moved to global scope */\n",
            vd->getName().str());
      break;
    }

    case DeclTreatment::NewGlobal: {
      // Replace the local declaration with flat assignments
      // For arrays we recurse into the init-list to produce one statement
      // per scalar element, handling arbitrary nesting depth
      if (vd->hasInit()) {
        auto *init = vd->getInit();
        if (vd->getType()->isArrayType()) {
          emitNestedArrayInits(init, entry.newName, "", pp, replacement);
        } else {
          std::string init_str;
          llvm::raw_string_ostream os(init_str);
          init->printPretty(os, nullptr, pp);
          replacement += std::format("{} = {};\n", entry.newName, init_str);
        }
      }
      // If no init, then the global is already zero-initialised so we don't
      // need to do anything
      break;
    }
    } // switch
  }

  if (replacement.empty())
    replacement = std::format("/* hoisted */\n");

  addReplacement(DS->getSourceRange(), replacement);
  return true;
}
auto CollectReplacementsVisitor::VisitDeclRefExpr(clang::DeclRefExpr *DR)
    -> bool {
  if (auto *vd = dyn_cast<clang::VarDecl>(DR->getDecl())) {
    auto it = info.hoistMap.find(vd);
    if (it != info.hoistMap.end())
      addReplacement(DR->getSourceRange(), it->second.newName);
  }
  return true;
}
void CollectReplacementsVisitor::addReplacement(clang::SourceRange range,
                                                const std::string &text) {
  // Expand to account for macro expansions so we replace the spelling loc.
  auto char_range = clang::CharSourceRange::getTokenRange(range);
  clang::tooling::Replacement const repl(SM, char_range, text);
  if (auto err = Repls.add(repl)) {
    llvm::errs() << "Replacement conflict: " << llvm::toString(std::move(err))
                 << "\n";
  }
}

void HoistConsumer::HandleTranslationUnit(clang::ASTContext &Ctx) {
  clang::SourceManager  const&sm = Ctx.getSourceManager();
  clang::TranslationUnitDecl *tu = Ctx.getTranslationUnitDecl();

  // collect all variables that are arrays/address-taken
  HoistInfo info;
  CollectVisitor cv(info, Ctx);
  cv.TraverseDecl(tu);

  // copy source text to new file
  const llvm::MemoryBufferRef buf =
      sm.getBufferOrFake(sm.getMainFileID(), clang::SourceLocation{});
  std::string const source_text(buf.getBuffer());

  if (info.hoistMap.empty()) {
    emitToFile(source_text);
    return;
  }

  // find first func location to insert global vars before it
  clang::SourceLocation first_func_loc;
  for (clang::Decl *d : tu->decls()) {
    if (auto *fd = dyn_cast<clang::FunctionDecl>(d)) {
      if (!fd->hasBody())
        continue;
      clang::SourceLocation const loc = fd->getSourceRange().getBegin();
      if (!sm.isInMainFile(loc))
        continue;
      if (first_func_loc.isInvalid() ||
          sm.isBeforeInTranslationUnit(loc, first_func_loc))
        first_func_loc = loc;
    }
  }

  clang::tooling::Replacements repls;

  // insert global declarations before the first function definition
  if (first_func_loc.isValid()) {
    std::string global_block = "\n/* c2pancake: hoisted variables (arrays "
                               "and address-taken locals) begin */\n";

    for (auto &[VD, entry] : info.hoistMap) {
      switch (entry.treatment) {
      case DeclTreatment::NewGlobal:
        // omit init as it will be re-applied at each time the function is
        // called
        global_block += buildGlobalDecl(VD, entry.newName, Ctx, false) + "\n";
        break;

      case DeclTreatment::StaticGlobal: {
        // keep init only if it is a compile-time constant
        bool keep_init = false;
        if (VD->hasInit()) {
          clang::Expr::EvalResult result;
          keep_init = VD->getInit()->EvaluateAsRValue(result, Ctx);
        }
        global_block +=
            buildGlobalDecl(VD, entry.newName, Ctx, keep_init) + "\n";
        break;
      }

      case DeclTreatment::ExternRedecl: {
        // extern can be emitted multipile times without issue
        global_block += buildGlobalDecl(VD, entry.newName, Ctx, false) + "\n";
        break;
      }
      }
    }

    global_block += "/* c2pancake: hoisted variables (arrays and "
                    "address-taken locals) end */\n";

    // An insertion is modelled as a zero-length replacement at the offset.
    unsigned const insert_offset = sm.getFileOffset(first_func_loc);
    const clang::tooling::Replacement ins(sm.getFilename(first_func_loc),
                                          insert_offset, 0, global_block);
    if (auto err = repls.add(ins))
      llvm::errs() << "hoist_rewriter: insertion conflict: "
                   << llvm::toString(std::move(err)) << "\n";
  }

  // collect DeclStmt and DeclRefExpr replacements
  CollectReplacementsVisitor crv(repls, Ctx, info);
  crv.TraverseDecl(tu);

  // apply all replacements to the original source in one shot
  llvm::Expected<std::string> result =
      clang::tooling::applyAllReplacements(source_text, repls);
  if (!result) {
    llvm::errs() << "hoist_rewriter: failed to apply replacements: "
                 << llvm::toString(result.takeError()) << "\n";
    return;
  }

  emitToFile(*result);
}
void HoistConsumer::emitToFile(const std::string &text) const {

  std::error_code ec;
  llvm::raw_fd_ostream out(outputPath, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "hoist_rewriter: cannot open '" << outputPath
                 << "': " << ec.message() << "\n";
    return;
  }
  out << text;
  llvm::errs() << "hoist_rewriter: wrote '" << outputPath << "'\n";
}

auto HoistAction::CreateASTConsumer(clang::CompilerInstance &CI,
                                    clang::StringRef file)
    -> std::unique_ptr<clang::ASTConsumer> {
  auto consumer = std::make_unique<HoistConsumer>(CI);
  consumer->current_suffix = cur_suffix_;
  auto original = file.drop_back(
      cur_suffix_.size()); // remove current suffix to get original filename
  consumer->outputPath = std::format("{}{}", original.str(), next_suffix_);
  return consumer;
}
