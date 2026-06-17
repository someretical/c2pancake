#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include <string>

auto exprToString(const clang::Expr *E, const clang::SourceManager &SM,
                  const clang::LangOptions &LO) -> std::string;
auto stmtToString(const clang::Stmt *S, const clang::SourceManager &SM,
                  const clang::LangOptions &LO) -> std::string;
auto hasSideEffect(const clang::Expr *E, const clang::ASTContext &Ctx) -> bool;
auto isTopLevelStmt(const clang::UnaryOperator *UO,
                    const clang::ASTContext &Ctx) -> bool;

#endif // C2PANCAKE_UTILS_H