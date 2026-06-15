#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include <string>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"

using namespace clang;

std::string exprToString(const Expr *E, const SourceManager &SM,
                         const LangOptions &LO);
std::string stmtToString(const Stmt *S, const SourceManager &SM,
                         const LangOptions &LO);
bool hasSideEffect(const Expr *E, ASTContext &Ctx);
bool isTopLevelStmt(const UnaryOperator *UO, ASTContext &Ctx);

#endif // C2PANCAKE_UTILS_H