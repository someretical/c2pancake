#include "utils.h"

#include "clang/AST/ParentMapContext.h"

std::string exprToString(const Expr *E, const SourceManager &SM,
                         const LangOptions &LO) {
  CharSourceRange R = CharSourceRange::getTokenRange(E->getSourceRange());
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(R, SM, LO, &Invalid);
  if (Invalid)
    return "<invalid>";
  return Text.str();
}

std::string stmtToString(const Stmt *S, const SourceManager &SM,
                         const LangOptions &LO) {
  CharSourceRange R = CharSourceRange::getTokenRange(S->getSourceRange());
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(R, SM, LO, &Invalid);
  if (Invalid)
    return "<invalid>";
  return Text.str();
}

bool hasSideEffect(const Expr *E, ASTContext &Ctx) {
  return E->HasSideEffects(Ctx, true);
}

bool isTopLevelStmt(const UnaryOperator *UO, ASTContext &Ctx) {
  DynTypedNodeList Parents = Ctx.getParents(*UO);
  for (const DynTypedNode &P : Parents) {
    if (P.get<Expr>() == nullptr)
      return true;
  }
  return false;
}
