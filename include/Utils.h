#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <string>

using namespace clang;

auto exprToString(const Expr *E, const SourceManager &SM, const LangOptions &LO)
    -> std::string;
auto stmtToString(const Stmt *S, const SourceManager &SM, const LangOptions &LO)
    -> std::string;
auto hasSideEffect(const Expr *E, ASTContext &Ctx) -> bool;
auto isTopLevelStmt(const UnaryOperator *UO, ASTContext &Ctx) -> bool;

#endif // C2PANCAKE_UTILS_H