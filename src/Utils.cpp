#include "Utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <string>

auto exprToString(const clang::Expr *E, const clang::SourceManager &SM,
                  const clang::LangOptions &LO) -> std::string {
  auto r = clang::CharSourceRange::getTokenRange(E->getSourceRange());
  bool invalid = false;
  auto text = clang::Lexer::getSourceText(r, SM, LO, &invalid);
  if (invalid)
    return "<invalid>";
  return text.str();
}

auto stmtToString(const clang::Stmt *S, const clang::SourceManager &SM,
                  const clang::LangOptions &LO) -> std::string {
  auto r = clang::CharSourceRange::getTokenRange(S->getSourceRange());
  bool invalid = false;
  auto text = clang::Lexer::getSourceText(r, SM, LO, &invalid);
  if (invalid)
    return "<invalid>";
  return text.str();
}

auto hasSideEffect(const clang::Expr *E, clang::ASTContext &Ctx) -> bool {
  return E->HasSideEffects(Ctx, true);
}

auto isTopLevelStmt(const clang::UnaryOperator *UO, clang::ASTContext &Ctx)
    -> bool {
  clang::DynTypedNodeList const parents = Ctx.getParents(*UO);
  return std::ranges::any_of(parents, [](const clang::DynTypedNode &p) -> bool {
    return p.get<clang::Stmt>() != nullptr;
  });
}
