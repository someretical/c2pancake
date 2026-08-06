#include "Pass_HoistArraysAndAddresses.h"
#include "Utils.h"

#include <clang-tools-extra/clangd/FindTarget.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::clangd;
using namespace clang::tooling;

namespace pancake::pass_rename_to_be_hoisted_globals {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::DenseMap<VarDecl *, Replacements> &hoisted_vars;
  size_t tmp_var_counter = 0;
  FunctionDecl *current_function_decl = nullptr;
  llvm::Error error = llvm::Error::success();
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner switch statements first, then the outermost one
  static auto shouldTraversePostOrder() -> bool { return false; }

  auto GetTempVarName(const StringRef original_name, const StringRef func_name, const SourceLocation loc) -> auto {
    return llvm::formatv("__c2pnk_local_{0}_{1}_L{2}C{3}_{4}_{5}_{6}", original_name.str(), func_name.str(),
                         data.Ctx.getSourceManager().getSpellingLineNumber(loc),
                         data.Ctx.getSourceManager().getSpellingColumnNumber(loc), data.ps_ctx.major_pass_number,
                         data.ps_ctx.minor_pass_number, data.tmp_var_counter++);
  }

  auto GetTempVarName(const std::string &hint) const {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.ps_ctx.major_pass_number, data.ps_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    auto *tmp_function_decl = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto cleanup =
        llvm::scope_exit([this, tmp_function_decl] -> void { data.current_function_decl = tmp_function_decl; });

    return RecursiveASTVisitor<Worker>::TraverseFunctionDecl(func_decl);
  }

  auto VisitVarDecl(VarDecl *var_decl) -> bool {
    if (data.error) {
      return false;
    }

    if (data.current_function_decl == nullptr) {
      return true;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(var_decl->getBeginLoc()))) {
      return true;
    }

    // check if static
    if (var_decl->getStorageClass() == SC_Static) {
      data.error = CreateRuntimeError(
          llvm::formatv("\n    at {0}\nStatic variable {1} found in function {2}. Static variables are not supported.",
                        var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName(),
                        data.current_function_decl->getName()));
      return false;
    }

    auto var_name = var_decl->getName();
    if (var_name.starts_with("__c2pnk_local_")) {
      // don't rewrite pancake helper variables
      return true;
    }

    if (data.hoisted_vars.contains(var_decl)) {
      data.error = CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nVariable {1} found in function {2} has already been hoisted. This should not happen.",
          var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_name,
          data.current_function_decl->getName())));
      return false;
    }

    auto tmp_var_name = GetTempVarName(var_name, data.current_function_decl->getName(), var_decl->getBeginLoc()).str();
    if (var_decl->getType()->isArrayType()) {
      Replacements repls;
      const auto *canonical_target = var_decl->getCanonicalDecl();
      findExplicitReferences(
          data.Ctx,
          [&](const ReferenceLoc &ref) -> void {
            for (const auto *target : ref.Targets) {
              if (const auto *target_vd = dyn_cast<VarDecl>(target)) {
                if (target_vd->getCanonicalDecl() == canonical_target) {
                  if (auto error = repls.add({data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(ref.NameLoc),
                                              tmp_var_name, data.Ctx.getLangOpts()})) {
                    data.error =
                        llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                             "\n    at {0}\nFailed to add rewrite for to-be-hoisted array variable {1}",
                                             ref.NameLoc.printToString(data.Ctx.getSourceManager()), var_name))),
                                         std::move(error));
                    return;
                  }
                }
              }
            }
          },
          nullptr);

      if (data.error) {
        return false;
      }

      data.hoisted_vars.insert({var_decl, std::move(repls)});
    } else if (var_decl->getType()->isRecordType()) {
      Replacements repls;
      const auto *canonical_target = var_decl->getType()->getAsRecordDecl()->getCanonicalDecl();
      if (canonical_target == nullptr) {
        data.error = CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nVariable {1} found in function {2} has a record type with "
                                    "no canonical decl. This should not happen.",
                                    var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_name,
                                    data.current_function_decl->getName())));
        return false;
      }

      findExplicitReferences(
          data.Ctx,
          [&](const ReferenceLoc &ref) -> void {
            for (const auto *target : ref.Targets) {
              if (const auto *target_rd = dyn_cast<RecordDecl>(target)) {
                if (target_rd->getCanonicalDecl() == canonical_target) {
                  if (auto error = repls.add({data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(ref.NameLoc),
                                              tmp_var_name, data.Ctx.getLangOpts()})) {
                    data.error =
                        llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                             "\n    at {0}\nFailed to add rewrite for to-be-hoisted array variable {1}",
                                             ref.NameLoc.printToString(data.Ctx.getSourceManager()), var_name))),
                                         std::move(error));
                    return;
                  }
                }
              }
            }
          },
          nullptr);

      if (data.error) {
        return false;
      }

      data.hoisted_vars.insert({var_decl, std::move(repls)});
    }
    return true;
  }

  auto VisitUnaryOperator(UnaryOperator *unary_operator) -> bool {
    if (data.error) {
      return false;
    }

    if (data.current_function_decl == nullptr) {
      return true;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(unary_operator->getBeginLoc()))) {
      return true;
    }

    if (unary_operator->getOpcode() == UO_AddrOf) {
      auto *operand = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      if (auto *decl_ref_expr = llvm::dyn_cast<DeclRefExpr>(operand)) {
        if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl_ref_expr->getDecl())) {
          auto var_name = var_decl->getName();
          if (!data.hoisted_vars.contains(var_decl) && !var_name.starts_with("__c2pnk_local_")) {
            Replacements repls;
            const auto *canonical_target = var_decl->getCanonicalDecl();
            auto tmp_var_name =
                GetTempVarName(var_name, data.current_function_decl->getName(), unary_operator->getBeginLoc()).str();
            findExplicitReferences(
                data.Ctx,
                [&](const ReferenceLoc &ref) -> void {
                  for (const auto *target : ref.Targets) {
                    if (const auto *target_vd = dyn_cast<VarDecl>(target)) {
                      if (target_vd->getCanonicalDecl() == canonical_target) {
                        if (auto error =
                                repls.add({data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(ref.NameLoc),
                                           tmp_var_name, data.Ctx.getLangOpts()})) {
                          data.error = llvm::joinErrors(
                              CreateRuntimeError(std::move(llvm::formatv(
                                  "\n    at {0}\nFailed to add rewrite for to-be-hoisted addrof variable {1}",
                                  ref.NameLoc.printToString(data.Ctx.getSourceManager()), var_name))),
                              std::move(error));
                          return;
                        }
                      }
                    }
                  }
                },
                nullptr);

            if (data.error) {
              return false;
            }

            data.hoisted_vars.insert({var_decl, std::move(repls)});
          }
        }
      }
    }
    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::DenseMap<VarDecl *, Replacements> hoisted_vars;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .hoisted_vars = hoisted_vars};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  if (hoisted_vars.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }

  Replacements replacements;
  for (const auto &[var_decl, repls] : hoisted_vars) {
    for (const auto &r : repls) {
      if (auto error = replacements.add(r)) {
        ps_ctx.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to add rewrite for to-be-hoisted variable {1}",
                var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName()))),
            std::move(error));
        ps_ctx.whats_next = WhatsNext::MoveToNextFile;
        return;
      }
    }
  }

  ps_ctx.replacements = std::move(replacements);
  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_rename_to_be_hoisted_globals

namespace pancake::pass_hoist_locals {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::DenseMap<FunctionDecl *, Replacements> &replacements;
  llvm::DenseMap<FunctionDecl *, llvm::SmallVector<VarDecl *, 16>> function_prologues;
  llvm::Error error = llvm::Error::success();
  FunctionDecl *current_function_decl = nullptr;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  static auto shouldTraversePostOrder() -> bool { return false; }

  auto PrintType(llvm::raw_ostream &os, const QualType ty, const llvm::StringRef var_name) const {
    ty.print(os, data.Ctx.getPrintingPolicy(), var_name);
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    auto *tmp = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto cleanup = llvm::scope_exit([&] -> void { data.current_function_decl = tmp; });

    auto res = RecursiveASTVisitor::TraverseFunctionDecl(func_decl);

    if (data.error) {
      return false;
    }

    auto it = data.function_prologues.find(func_decl);
    if (it != data.function_prologues.end()) {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      os << "\n/* c2pancake: promoted variable declarations for function " << func_decl->getName() << " BEGIN */\n";
      for (const auto *var_decl : it->second) {
        PrintType(os, var_decl->getType(), var_decl->getName());
        os << ";\n";
      }
      os << "/* c2pancake: promoted variable declarations for function " << func_decl->getName() << " END */\n";
      os.flush();
      // insert before start of function...
      if (auto error = data.replacements[func_decl].add(
              {data.Ctx.getSourceManager(), func_decl->getBeginLoc(), 0, replacement_text})) {
        data.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to add rewrite for promoted variable declarations for function {1}",
                func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()))),
            std::move(error));
        return false;
      }
    }

    return res;
  }

  auto HandleVarDecl(VarDecl *var_decl) -> llvm::Error {
    auto name = var_decl->getName();
    if (name.starts_with("__c2pnk_local_")) {
      data.function_prologues[data.current_function_decl].push_back(var_decl);
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      if (var_decl->hasInit()) {
        auto init_source_text = GetSourceText(var_decl->getInit(), data.Ctx);
        if (auto error = init_source_text.takeError()) {
          return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                      "\n    at {0}\nFailed to get source text for initializer of variable {1}",
                                      var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), name))),
                                  std::move(error));
        }
        os << llvm::formatv("{0} = {1}", var_decl->getName(), *init_source_text);
        os.flush();
      }

      if (auto error = data.replacements[data.current_function_decl].add(
              {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(var_decl->getSourceRange()),
               replacement_text, data.Ctx.getLangOpts()})) {
        return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                    "\n    at {0}\nFailed to add rewrite for promoted variable declaration "
                                    "for variable {1} in function {2}",
                                    var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), name,
                                    data.current_function_decl->getName()))),
                                std::move(error));
      }
    }

    return llvm::Error::success();
  }

  auto VisitDeclStmt(DeclStmt *decl_stmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (decl_stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(decl_stmt->getBeginLoc())) ||
        data.current_function_decl == nullptr) {
      return true;
    }

    if (decl_stmt->isSingleDecl()) {
      if (auto *var_decl = dyn_cast<VarDecl>(decl_stmt->getSingleDecl())) {
        if (auto error = HandleVarDecl(var_decl)) {
          data.error = std::move(error);
          return false;
        }
      }
    } else {
      data.error = CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nDeclStmt with multiple declarations found in function {1}. This should not happen.",
          decl_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()), data.current_function_decl->getName())));
      return false;
    }

    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::DenseMap<FunctionDecl *, Replacements> hoisted_vars;
  llvm::DenseMap<FunctionDecl *, llvm::SmallVector<VarDecl *, 16>> const function_prologues;
  WorkerData data{.Ctx = Ctx,
                  .ps_ctx = ps_ctx,
                  .replacements = hoisted_vars,
                  .function_prologues = function_prologues,
                  .current_function_decl = nullptr};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  Replacements replacements;
  for (const auto &[func_decl, repls] : hoisted_vars) {
    for (const auto &r : repls) {
      if (auto error = replacements.add(r)) {
        ps_ctx.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to add rewrite for hoisted variable in function {1}",
                func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()))),
            std::move(error));
        ps_ctx.whats_next = WhatsNext::MoveToNextFile;
        return;
      }
    }
  }

  ps_ctx.replacements = std::move(replacements);
  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_hoist_locals
