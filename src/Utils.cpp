#include "Utils.h"

#include "clang/AST/ParentMapContext.h"
#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <string>

auto exprToString(const Expr *E, const SourceManager &SM, const LangOptions &LO)
    -> std::string {
  CharSourceRange const r = CharSourceRange::getTokenRange(E->getSourceRange());
  bool invalid = false;
  StringRef text = Lexer::getSourceText(r, SM, LO, &invalid);
  if (invalid)
    return "<invalid>";
  return text.str();
}

auto stmtToString(const Stmt *S, const SourceManager &SM, const LangOptions &LO)
    -> std::string {
  CharSourceRange const r = CharSourceRange::getTokenRange(S->getSourceRange());
  bool invalid = false;
  StringRef text = Lexer::getSourceText(r, SM, LO, &invalid);
  if (invalid)
    return "<invalid>";
  return text.str();
}

auto hasSideEffect(const Expr *E, ASTContext &Ctx) -> bool {
  return E->HasSideEffects(Ctx, true);
}

auto isTopLevelStmt(const UnaryOperator *UO, ASTContext &Ctx) -> bool {
  DynTypedNodeList const parents = Ctx.getParents(*UO);
  return std::ranges::any_of(parents, [](const DynTypedNode &p) -> bool {
    return p.get<Stmt>() != nullptr;
  });
}
