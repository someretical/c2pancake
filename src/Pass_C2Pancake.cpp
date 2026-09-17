#include "Pass_C2Pancake.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Inclusions/HeaderIncludes.h>
#include <clang/Tooling/Inclusions/IncludeStyle.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <utility>

using namespace clang;
using namespace clang::tooling;

namespace pancake::pass_c2pancake {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
};

enum class Usage : uint8_t {
  /**
   * @brief Compute the expression's resulting value
   *
   * Used when: The caller needs the result
   *
   * Example: x = a + b, foo(expr), int y = (b = 3)
   */
  Value,

  /**
   * @brief Compute the reusable description of the storage location of the expression. Note this is very specifically
   * not supposed to return a pointer!
   *
   * Used when: The caller needs to read/write memory at that location
   *
   * Example: x = 3 (lhs), *p = v, ++a->b
   */
  Place,

  /**
   * @brief Execute the expression only for its side effects; ignore its value
   *
   * Used when: The result is thrown away
   *
   * Example: a = 3;, foo(expr);, ++a;
   */
  Effect
};

struct BuildExprCtx {
  Expr *expr;
  Usage usage_kind;
  explicit BuildExprCtx(Expr *expr, Usage usage_kind) : expr(expr), usage_kind(usage_kind) {}
};

struct BuiltExpr {
  llvm::SmallVector<std::string, 8> pre_stmts;
  clang::QualType final_expr_type;
  std::string final_expr;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr(std::move(final_expr)) {}
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return true; }

  auto TraverseTranslationUnitDecl(TranslationUnitDecl *tu_decl) -> bool {
    if (data.error) {
      return false;
    }

    for (auto *decl : tu_decl->decls()) {
      if (auto *func_decl = llvm::dyn_cast<FunctionDecl>(decl)) {
        if (!func_decl->isThisDeclarationADefinition()) {
          continue;
        }

        if (!TraverseFunctionDecl(func_decl)) {
          return false;
        }
        continue;
      }

      if (auto *_ = llvm::dyn_cast<TypedefDecl>(decl)) {
        continue;
      }

      if (auto *_ = llvm::dyn_cast<RecordDecl>(decl)) {
        continue;
      }

      data.error = CreateRuntimeError("Unexpected top-level declaration\n    at " +
                                      decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
      return false;
    }

    return true;
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    if (!func_decl->isThisDeclarationADefinition()) {
      return true;
    }

    auto *body = func_decl->getBody();
    if (body != nullptr) {
      if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(body)) {
        for (auto *stmt : compound_stmt->body()) {
          if (!TraverseStmt(stmt)) {
            return false;
          }
        }
      } else {
        data.error = CreateRuntimeError("Function body is not a compound statement\n    at " +
                                        body->getBeginLoc().printToString(data.Ctx.getSourceManager()));
        return false;
      }
    }

    // TODO output pancake equivalent of the function
    // the body should already be handled by TraverseStmt

    return false;
  }

  auto TraverseStmt(Stmt *stmt) -> Expected<std::string> {
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(stmt)) {
      for (auto *sub_stmt : compound_stmt->body()) {
        auto result = TraverseStmt(sub_stmt);
        if (auto error = result.takeError()) {
          return std::move(error);
        }

        os << *result;
      }
      os.flush();

      return replacement_text;
    }

    if (auto *decl_stmt = llvm::dyn_cast<DeclStmt>(stmt)) {
      for (auto *decl : decl_stmt->decls()) {
        if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
          // TODO handle var decl
          continue;
        }

        return CreateRuntimeError("Unexpected declaration statement\n    at " +
                                  decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
      }

      return replacement_text;
    }

    return CreateRuntimeError("Unexpected statement\n    at " +
                              stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()));
  }

  auto TraverseExpr(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    return CreateRuntimeError("Unexpected expression\n    at " +
                              ctx.expr->getBeginLoc().printToString(data.Ctx.getSourceManager()));
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

  for (const auto &r : replacements) {
    if (auto error = ps_ctx.replacements.add(r)) {
      ps_ctx.error = llvm::joinErrors(CreateRuntimeError("Add replacement conflict"), std::move(error));
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_c2pancake
