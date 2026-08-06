#include "Pass_NormaliseIfStatements.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;

namespace pancake::pass_normalise_if_statements {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t if_cond_tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner if statements first, then the outermost one
  static auto shouldTraversePostOrder() -> bool { return true; }

  auto GetIfCondTempVarName() -> auto {
    return llvm::formatv("__c2pnk_if_cond_tmp_var_{0}_{1}_{2}", data.ps_ctx.major_pass_number,
                         data.ps_ctx.minor_pass_number, data.if_cond_tmp_var_counter++);
  }

  auto IsPartOfElseIfChain(const IfStmt *if_stmt) -> bool {
    auto parents = data.Ctx.getParentMapContext().getParents(*if_stmt);
    if (parents.empty())
      return false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (const auto *parent_if = parents[0].get<clang::IfStmt>()) {
      if (parent_if->getElse() == if_stmt) {
        return true;
      }
    }
    return false;
  }

  auto ConvertIfStmt(IfStmt *if_stmt) -> llvm::Error {
    auto &sm = data.Ctx.getSourceManager();
    if (if_stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(if_stmt->getBeginLoc()))) {
      return llvm::Error::success();
    }

    const auto *cond = if_stmt->getCond()->IgnoreParenImpCasts();
    const auto *then_stmt = if_stmt->getThen();
    const auto *else_stmt = if_stmt->getElse();

    bool needs_rewrite = true;
    bool needs_cond_hoist = false;
    if (const auto *decl_ref_expr = dyn_cast<clang::DeclRefExpr>(cond)) {
      if (llvm::isa<clang::VarDecl>(decl_ref_expr->getDecl())) {
        // pure var reference, no need to rewrite
        needs_rewrite = false;
      } else {
        needs_cond_hoist = true;
      }
    } else {
      needs_cond_hoist = true;
    }

    // no else statement, no need to rewrite
    if (else_stmt == nullptr) {
      needs_rewrite = false;
    }

    // else statement is not an else-if statement, no need to rewrite
    if (else_stmt != nullptr && !isa<clang::IfStmt>(else_stmt)) {
      needs_rewrite = false;
    }

    if (!needs_rewrite && !needs_cond_hoist) {
      return llvm::Error::success();
    }

    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    auto original_cond_text = GetSourceText(cond, data.Ctx);
    if (auto error = original_cond_text.takeError()) {
      return llvm::joinErrors(
          CreateRuntimeError(std::move(llvm::formatv("\n    at {0}\nFailed to get source text for IfStmt condition",
                                                     cond->getExprLoc().printToString(data.Ctx.getSourceManager())))),
          std::move(error));
    }
    const std::string cond_name = needs_cond_hoist ? GetIfCondTempVarName() : *original_cond_text;

    // if this if-statement is part of an else-if chain, we need to wrap it in a compound statement no matter what
    if (IsPartOfElseIfChain(if_stmt)) {
      os << "{\n";
    }

    if (needs_cond_hoist) {
      os << llvm::formatv("{0} {1} = ({2});\n", GetWordTypeStr(data.Ctx), cond_name, *original_cond_text);
    }

    os << llvm::formatv("if ({0}) ", cond_name);
    if (const auto *then_compound_stmt = dyn_cast<CompoundStmt>(then_stmt)) {
      if (auto error = PrintSourceText(os, then_compound_stmt, data.Ctx)) {
        return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                    "\n    at {0}\nFailed to print source text for CompoundStmt",
                                    then_compound_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                std::move(error));
      }
    } else {
      os << "{\n";
      if (auto error = PrintSourceText(os, then_stmt, data.Ctx)) {
        return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                    "\n    at {0}\nFailed to print source text for statement",
                                    then_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                std::move(error));
      }
      os << ";\n}";
    }

    if (else_stmt != nullptr) {
      os << " else ";
      if (const auto *else_compound_stmt = dyn_cast<CompoundStmt>(else_stmt)) {
        if (auto error = PrintSourceText(os, else_compound_stmt, data.Ctx)) {
          return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                      "\n    at {0}\nFailed to print source text for CompoundStmt",
                                      else_compound_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                  std::move(error));
        }
      } else {
        os << "{\n";
        if (auto error = PrintSourceText(os, else_stmt, data.Ctx)) {
          return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                      "\n    at {0}\nFailed to print source text for statement",
                                      else_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                  std::move(error));
        }
        os << "\n}";
      }
    }

    if (IsPartOfElseIfChain(if_stmt)) {
      os << "\n}";
    }

    os.flush();
    data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                   CharSourceRange::getTokenRange(if_stmt->getSourceRange()), replacement_text,
                                   data.Ctx.getLangOpts());

    return llvm::Error::success();
  }

  auto VisitIfStmt(IfStmt *ifStmt) -> bool {
    if (data.error) {
      return false;
    }

    if (auto error = ConvertIfStmt(ifStmt)) {
      data.error = std::move(error);
      return false;
    }

    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  int errors = 0;
  for (const auto &r : replacements) {
    if (auto error = ps_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(error));
      errors++;
    }
  }

  if (errors == 0 && replacements.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_normalise_if_statements
