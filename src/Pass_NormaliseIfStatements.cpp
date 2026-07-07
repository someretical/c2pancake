#include "Pass_NormaliseIfStatements.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/SourceCode.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace pancake::pass_normalise_if_statements {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineActionCtx &pa_ctx;
  std::vector<Replacement> &replacements;
  size_t if_cond_tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner if statements first, then the outermost one
  auto shouldTraversePostOrder() const -> bool { return true; }

  auto GetIfCondTempVarName() -> auto {
    return llvm::formatv("__c2pnk_if_cond_tmp_var_{0}_{1}_{2}", data.pa_ctx.major_pass_number,
                         data.pa_ctx.minor_pass_number, data.if_cond_tmp_var_counter++);
  }

  auto ConvertIfStmt(IfStmt *ifStmt) -> void {
    auto &sm = data.Ctx.getSourceManager();
    if (ifStmt == nullptr || sm.isInSystemHeader(sm.getSpellingLoc(ifStmt->getBeginLoc()))) {
      return;
    }

    const auto *cond = ifStmt->getCond()->IgnoreParenImpCasts();
    const auto *then_stmt = ifStmt->getThen();
    const auto *else_stmt = ifStmt->getElse();

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
      return;
    }

    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    const std::string original_cond_text = Lexer::getSourceText(CharSourceRange::getTokenRange(cond->getSourceRange()),
                                                                data.Ctx.getSourceManager(), data.Ctx.getLangOpts())
                                               .str();
    const std::string cond_name = needs_cond_hoist ? GetIfCondTempVarName() : original_cond_text;
    if (needs_cond_hoist) {
      os << llvm::formatv("int {0} = ({1});\n", cond_name, original_cond_text);
    }

    os << llvm::formatv("if ({0}) ", cond_name);
    if (const auto *then_compound_stmt = dyn_cast<CompoundStmt>(then_stmt)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(then_compound_stmt->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else {
      os << "{\n";
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(then_stmt->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
      os << ";\n}";
    }

    if (else_stmt != nullptr) {
      os << " else ";
      if (const auto *else_compound_stmt = dyn_cast<CompoundStmt>(else_stmt)) {
        os << Lexer::getSourceText(CharSourceRange::getTokenRange(else_compound_stmt->getSourceRange()),
                                   data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
      } else {
        os << "{\n";
        os << Lexer::getSourceText(CharSourceRange::getTokenRange(else_stmt->getSourceRange()),
                                   data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
        os << "\n}";
      }
    }

    data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                   CharSourceRange::getTokenRange(ifStmt->getSourceRange()), os.str());
  }

  auto VisitIfStmt(IfStmt *ifStmt) -> bool {
    ConvertIfStmt(ifStmt);

    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  std::vector<Replacement> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = pa_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  bool add_error_occurred = false;
  for (const auto &r : replacements) {
    if (auto err = pa_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
      add_error_occurred = true;
    }
  }

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_normalise_if_statements
