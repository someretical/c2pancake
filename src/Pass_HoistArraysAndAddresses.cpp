#include "Pass_HoistArraysAndAddresses.h"
#include "Utils.h"

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
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace pancake::pass_hoist_arrays_and_addresses;

namespace {
// generate a unique global name for a hoisted array variable
auto ArrayPrefix(bool make_shared, size_t counter, llvm::StringRef func, llvm::StringRef var) -> std::string {
  return llvm::formatv("__c2pnk_{0}_array_{1}_{2}_{3}", make_shared ? "shared" : "local", func, var, counter).str();
}

// generate a unique global name for a hoisted address-taken variable
auto PtrPrefix(bool make_shared, size_t counter, llvm::StringRef func, llvm::StringRef var) -> std::string {
  return llvm::formatv("__c2pnk_{0}_ptr_{1}_{2}_{3}", make_shared ? "shared" : "local", func, var, counter).str();
}

auto RecordPrefix(bool make_shared, size_t counter, llvm::StringRef func, llvm::StringRef var) -> std::string {
  return llvm::formatv("__c2pnk_{0}_record_{1}_{2}_{3}", make_shared ? "shared" : "local", func, var, counter).str();
}

// Nested array init helper
//
// Walks an InitListExpr recursively, tracking the subscript path (e.g.
// "[0][2]") and emitting one flat assignment statement per scalar leaf.
// This also works for structs
//
//   float mat[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
//   ->
//   __hoist_arr_matrix_mat_0[0][0] = 1;
//   __hoist_arr_matrix_mat_0[0][1] = 0;
// NOLINTNEXTLINE(misc-no-recursion)
void EmitNestedInits(const clang::Expr *expr, const clang::Type *type, llvm::StringRef baseName,
                     llvm::StringRef accessPath, const clang::PrintingPolicy &pp, clang::ASTContext &Ctx,
                     llvm::raw_ostream &out) {
  const auto *stripped = expr->IgnoreParenCasts();
  const auto *ile = dyn_cast<clang::InitListExpr>(stripped);

  if (ile == nullptr) {
    // Scalar leaf — emit the assignment
    std::string val;
    llvm::raw_string_ostream os(val);
    expr->printPretty(os, nullptr, pp);
    out << llvm::formatv("{0}{1} = {2};\n", baseName, accessPath, val);
    return;
  }

  const auto *to_walk = ile->isSyntacticForm() ? ile : ile->getSyntacticForm();
  if (to_walk == nullptr)
    to_walk = ile;

  if (to_walk->getNumInits() == 0)
    return;

  if (type->isArrayType()) {
    const auto *at = Ctx.getAsArrayType(clang::QualType(type, 0));
    const clang::Type *elem_type = at->getElementType().getTypePtr();

    for (unsigned i = 0; i < to_walk->getNumInits(); ++i) {
      llvm::SmallString<64> path;
      llvm::raw_svector_ostream(path) << accessPath << "[" << i << "]";
      EmitNestedInits(to_walk->getInit(i), elem_type, baseName, path, pp, Ctx, out);
    }
  } else if (type->isRecordType()) {
    const auto *rd = type->getAs<clang::RecordType>()->getDecl();
    // Collect fields in order to match against init list positions
    llvm::SmallVector<const clang::FieldDecl *, 8> fields;
    for (const clang::FieldDecl *fd : rd->fields())
      fields.push_back(fd);

    for (unsigned i = 0; i < to_walk->getNumInits() && i < fields.size(); ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const clang::FieldDecl *fd = fields[i];
      llvm::SmallString<64> path;
      llvm::raw_svector_ostream(path) << accessPath << "." << fd->getName();
      EmitNestedInits(to_walk->getInit(i), fd->getType().getTypePtr(), baseName, path, pp, Ctx, out);
    }
  } else {
    // Shouldn't happen except for bitfields...
    std::string val;
    llvm::raw_string_ostream os(val);
    expr->printPretty(os, nullptr, pp);
    out << llvm::formatv("{0}{1} = {2};\n", baseName, accessPath, val);
  }
}

// Build a global declaration string for a VarDecl
auto BuildGlobalDecl(const clang::VarDecl *VD, llvm::StringRef newName, clang::ASTContext &Ctx, bool preserveInit)
    -> std::string {
  const auto &pp(Ctx.getLangOpts());
  const auto qt = VD->getType();
  const auto sc = VD->getStorageClass();
  llvm::SmallString<128> decl;
  llvm::raw_svector_ostream os(decl);
  os << ((sc == clang::SC_Extern) ? "extern " : "static ");
  qt.print(os, pp, newName);
  os << ";";

  if (preserveInit && VD->hasInit()) {
    os << llvm::formatv("\n/* c2pancake: initialiser for static variable {0} was not a "
                        "compile-time constant, so it was emitted as runtime code */\n",
                        newName);
    os << "/* c2pancake: move the following code into the entry point of the "
          "program */\n";
    EmitNestedInits(VD->getInit(), VD->getType().getTypePtr(), newName, "", pp, Ctx, os);
    os << llvm::formatv("/* c2pancake: end of initialiser for static variable {0} */\n", newName);
  }
  return std::string(decl);
}
} // namespace

auto PassFind::VisitUnaryOperator(clang::UnaryOperator *UO) -> bool {
  if (UO->getOpcode() == clang::UO_AddrOf) {
    auto *sub = UO->getSubExpr()->IgnoreParenCasts();
    if (auto *dr = dyn_cast<clang::DeclRefExpr>(sub))
      if (auto *vd = dyn_cast<clang::VarDecl>(dr->getDecl()))
        addressTaken.insert(vd);
  }
  return true;
}

auto PassFind::VisitMemberExpr(clang::MemberExpr *ME) -> bool {
  auto *fd = dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
  if ((fd == nullptr) || !fd->getType()->isArrayType())
    return true;

  // Strip & and -> to get to the base VarDecl
  const clang::Expr *base = ME->getBase()->IgnoreParenImpCasts();
  // handle p->arr where p is a pointer-to-struct local and nested structs
  while (true) {
    if (const auto *inner = dyn_cast<clang::MemberExpr>(base)) {
      base = inner->getBase()->IgnoreParenImpCasts();

    } else if (const auto *uo = dyn_cast<clang::UnaryOperator>(base);
               (uo != nullptr) && uo->getOpcode() == clang::UO_Deref) {
      base = uo->getSubExpr()->IgnoreParenImpCasts();
    } else {
      break;
    }
  }

  if (const auto *dr = dyn_cast<clang::DeclRefExpr>(base))
    if (const auto *vd = dyn_cast<clang::VarDecl>(dr->getDecl()))
      arrayFieldAccessed.insert(vd);

  return true;
}

auto PassAnalyse::VisitFunctionDecl(clang::FunctionDecl *FD) -> bool {
  if (!FD->hasBody() || !FD->isThisDeclarationADefinition())
    return true;

  PassFind atf;
  atf.TraverseStmt(FD->getBody());
  WalkStmt(FD->getBody(), FD, atf);
  return true;
}

// NOLINTNEXTLINE(misc-no-recursion)
void PassAnalyse::WalkStmt(clang::Stmt *S, clang::FunctionDecl *FD, PassFind &atf) {
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
      const auto addr_taken_var = atf.addressTaken.contains(vd);
      const auto array_field_accessed_var = atf.arrayFieldAccessed.contains(vd);
      const auto *rt = qt->getAs<clang::RecordType>();
      const auto is_record = (rt != nullptr);

      if (sc == clang::SC_Extern) {
        info.hoistMap.insert({vd, VarHoistEntry{vd->getName().str(), DeclTreatment::ExternRedecl, FD}});
        continue;
      }

      if (sc == clang::SC_Static && (is_array || addr_taken_var)) {
        // static func vars are always shared across all threads
        const auto new_name = is_array ? ArrayPrefix(true, info.hoist_arr_counter++, FD->getName(), vd->getName())
                                       : PtrPrefix(true, info.hoist_ptr_counter++, FD->getName(), vd->getName());
        info.hoistMap.insert({vd, VarHoistEntry{new_name, DeclTreatment::StaticGlobal, FD}});
        continue;
      }

      if (vd->hasLocalStorage() && (is_array || addr_taken_var)) {
        const auto new_name = is_array ? ArrayPrefix(false, info.hoist_arr_counter++, FD->getName(), vd->getName())
                                       : PtrPrefix(false, info.hoist_ptr_counter++, FD->getName(), vd->getName());
        info.hoistMap.insert({vd, VarHoistEntry{new_name, DeclTreatment::NewGlobal, FD}});
      }

      if (vd->hasLocalStorage() && is_record) {
        if (addr_taken_var || array_field_accessed_var) {
          const auto new_name = RecordPrefix(false, info.hoist_record_counter++, FD->getName(), vd->getName());
          info.hoistMap.insert({vd, VarHoistEntry{new_name, DeclTreatment::NewGlobal, FD}});
          continue;
        }
      }

      if (sc == clang::SC_Static && is_record) {
        if (addr_taken_var || array_field_accessed_var) {
          const auto new_name = RecordPrefix(true, info.hoist_record_counter++, FD->getName(), vd->getName());
          info.hoistMap.insert({vd, VarHoistEntry{new_name, DeclTreatment::StaticGlobal, FD}});
          continue;
        }
      }
    }
  }

  for (auto *child : S->children())
    WalkStmt(child, FD, atf);
}

auto PassRename::VisitDeclStmt(clang::DeclStmt *DS) -> bool {
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
  llvm::SmallString<256> replacement;
  llvm::raw_svector_ostream out(replacement);

  for (auto &p : parts) {
    auto *vd = p.VD;

    if (p.entry == nullptr) {
      // wasn't hoisted
      std::string decl_str;
      llvm::raw_string_ostream os(decl_str);
      vd->print(os, pp);
      out << llvm::formatv("{0};\n", decl_str);
      continue;
    }

    const VarHoistEntry &entry = *p.entry;

    switch (entry.treatment) {

    case DeclTreatment::ExternRedecl:
      // Remove the local re-declaration; the external symbol already exists.
      out << llvm::formatv("\n\n/* c2pancake: extern decl {0} was moved to global scope */\n", vd->getName());
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
          out << llvm::formatv("{0} = {1};\n", entry.newName, init_str);
          emitted = true;
        }
      }
      if (!emitted)
        out << llvm::formatv("/* c2pancake: static decl {0} was moved to global scope */\n\n", vd->getName());
      break;
    }

    case DeclTreatment::NewGlobal: {
      if (vd->hasInit()) {
        auto *init = vd->getInit();
        EmitNestedInits(init, vd->getType().getTypePtr(), entry.newName, "", pp, Ctx, out);
      }
      // If no init, then the global is already zero-initialised so we don't
      // need to do anything
      break;
    }
    } // switch
  }

  llvm::StringRef const replacement_ref =
      replacement.empty() ? llvm::StringRef("/* hoisted */\n") : llvm::StringRef(replacement);

  AddReplacement(DS->getSourceRange(), replacement_ref);
  return true;
}
auto PassRename::VisitDeclRefExpr(clang::DeclRefExpr *DR) -> bool {
  if (auto *vd = dyn_cast<clang::VarDecl>(DR->getDecl())) {
    auto it = info.hoistMap.find(vd);
    if (it != info.hoistMap.end())
      AddReplacement(DR->getSourceRange(), it->second.newName);
  }
  return true;
}
void PassRename::AddReplacement(clang::SourceRange range, llvm::StringRef text) {
  // Expand to account for macro expansions so we replace the spelling loc.
  auto char_range = clang::CharSourceRange::getTokenRange(range);
  clang::tooling::Replacement const repl(SM, char_range, text);
  if (auto err = Repls.add(repl)) {
    llvm::errs() << llvm::formatv("Replacement conflict: {0}\n", llvm::fmt_consume(std::move(err)));
    has_replacement_error = true;
  }
}

void Consumer::HandleTranslationUnit(clang::ASTContext &Ctx) {
  const auto &sm = Ctx.getSourceManager();
  auto *tu = Ctx.getTranslationUnitDecl();

  // collect all variables that are arrays/address-taken
  HoistInfo info;
  PassAnalyse cv(info, Ctx);
  cv.TraverseDecl(tu);

  if (info.hoistMap.empty()) {
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
      if (first_func_loc.isInvalid() || sm.isBeforeInTranslationUnit(loc, first_func_loc))
        first_func_loc = loc;
    }
  }

  bool insertion_failed = false;

  // insert global declarations before the first function definition
  if (first_func_loc.isValid()) {
    llvm::SmallString<512> global_block;
    llvm::raw_svector_ostream gb(global_block);
    gb << "\n/* c2pancake: hoisted variables (arrays "
          "and address-taken locals) begin */\n";

    for (auto &[VD, entry] : info.hoistMap) {
      switch (entry.treatment) {
      case DeclTreatment::NewGlobal:
        // omit init as it will be re-applied at each time the function is
        // called
        gb << BuildGlobalDecl(VD, entry.newName, Ctx, false) << "\n";
        break;

      case DeclTreatment::StaticGlobal: {
        // keep init only if it is a compile-time constant
        bool keep_init = false;
        if (VD->hasInit()) {
          clang::Expr::EvalResult result;
          keep_init = VD->getInit()->EvaluateAsRValue(result, Ctx);
        }
        gb << BuildGlobalDecl(VD, entry.newName, Ctx, keep_init) << "\n";
        break;
      }

      case DeclTreatment::ExternRedecl: {
        // extern can be emitted multipile times without issue
        gb << BuildGlobalDecl(VD, entry.newName, Ctx, false) << "\n";
        break;
      }
      }
    }

    gb << "/* c2pancake: hoisted variables (arrays and "
          "address-taken locals) end */\n";

    // An insertion is modelled as a zero-length replacement at the offset.
    unsigned const insert_offset = sm.getFileOffset(first_func_loc);
    const clang::tooling::Replacement ins(sm.getFilename(first_func_loc), insert_offset, 0, global_block.str());
    if (auto err = pa_ctx.replacements.add(ins)) {
      llvm::errs() << llvm::formatv("{0} Replacement conflict\n", LogBegin(pa_ctx));

      insertion_failed = true;
    }
  }

  // collect DeclStmt and DeclRefExpr replacements
  PassRename crv(pa_ctx.replacements, Ctx, info);
  crv.TraverseDecl(tu);

  if (insertion_failed || crv.has_replacement_error) {
    llvm::errs() << llvm::formatv("{0} Aborting due to replacement conflicts, no output written for \n",
                                  LogBegin(pa_ctx));
    return;
  }
}
