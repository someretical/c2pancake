#ifndef C2PANCAKE_IRBUILDER_H
#define C2PANCAKE_IRBUILDER_H

#include "PancakeIR.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/DenseMap.h>

#include <functional>
#include <ranges>
#include <vector>

namespace pancake {
struct BuiltExpression {
  ExprPtr finalExpr; // can be converted into an expression statement!
  std::vector<StmtPtr> preStmts;
  std::vector<StmtPtr> postStmts;
};

class IRBuilder : public clang::RecursiveASTVisitor<IRBuilder> {
public:
  IRBuilder(const clang::Rewriter &R, clang::ASTContext &Ctx);

  auto getGlobals() const -> auto {
    return GlobalOrdering | std::views::reverse;
  }

  auto getFunctions() const -> auto {
    return FunctionOrdering | std::views::reverse;
  }

  // NOLINTBEGIN(misc-no-recursion,bugprone-derived-method-shadowing-base-method)
  // top level
  auto TraverseFunctionDecl(clang::FunctionDecl *FD) -> bool;
  // auto TraverseVarDecl(clang::VarDecl *VD) -> bool;
  // auto TraverseRecordDecl(clang::RecordDecl *RD) -> bool;
  // auto TraverseEnumDecl(clang::EnumDecl *ED) -> bool;

  // inside functions
  // auto TraverseParmVarDecl(clang::ParmVarDecl *PVD) -> bool;
  auto TraverseCompoundStmt(clang::CompoundStmt *CS) -> bool;

  // inside blocks
  auto TraverseDeclStmt(clang::DeclStmt *D) -> bool;
  // TraverseVarDecl can appear under DeclStmt!
  // auto TraverseBinaryOperator(clang::BinaryOperator *BO) -> bool;
  // auto TraverseUnaryOperator(clang::UnaryOperator *UO) -> bool;
  // auto TraverseCallExpr(clang::CallExpr *CE) -> bool;
  auto TraverseReturnStmt(clang::ReturnStmt *RS) -> bool;

  // things that appear under expressions:
  auto TraverseDeclRefExpr(clang::DeclRefExpr *DRE) -> bool;
  auto TraverseIntegerLiteral(clang::IntegerLiteral *IL) -> bool;
  // auto TraverseArraySubscriptExpr(clang::ArraySubscriptExpr *ASE) -> bool;
  // auto TraverseMemberExpr(clang::MemberExpr *ME) -> bool;
  // NOLINTEND(misc-no-recursion,bugprone-derived-method-shadowing-base-method)

private:
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
  const clang::Rewriter &Rewriter;
  const clang::ASTContext &Ctx;
  const clang::LangOptions &LO;
  const clang::SourceManager &SM;
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

  clang::FunctionDecl *CurrentFunction = nullptr;

  llvm::DenseMap<const clang::FunctionDecl *, Function> IR_FunctionMap;
  llvm::DenseMap<const clang::Decl *, std::vector<StmtPtr>> IR_DeclMap;
  llvm::DenseMap<const clang::Stmt *, std::vector<StmtPtr>> IR_GlobalStmts;
  llvm::DenseMap<const clang::Stmt *, std::vector<StmtPtr>> IR_BuiltStmts;
  llvm::DenseMap<const clang::Stmt *, BuiltExpression> IR_BuiltExprs;

  std::vector<std::reference_wrapper<const StmtPtr>> GlobalOrdering;
  std::vector<std::reference_wrapper<const Function>> FunctionOrdering;
};
} // namespace pancake

#endif // C2PANCAKE_IRBUILDER_H