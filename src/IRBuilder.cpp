#include "IRBuilder.h"
#include "PancakeIR.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <utility>
#include <vector>

using namespace pancake;

IRBuilder::IRBuilder(const clang::Rewriter &R, clang::ASTContext &Ctx)
    : Rewriter(R), Ctx(Ctx), LO(Ctx.getLangOpts()), SM(Ctx.getSourceManager()) {
}

auto IRBuilder::TraverseFunctionDecl(clang::FunctionDecl *FD) -> bool {
  CurrentFunction = FD;
  auto guard =
      llvm::scope_exit([this] -> void { this->CurrentFunction = nullptr; });

  if (!RecursiveASTVisitor::TraverseFunctionDecl(FD))
    return false;

  if (!FD->isThisDeclarationADefinition())
    return true;

  auto *key = FD;
  auto location = FD->getLocation();
  auto name = FD->getName().str();
  auto parm_var_decls = FD->parameters();

  std::vector<Param> args;
  std::ranges::transform(parm_var_decls, std::back_inserter(args),
                         [](clang::ParmVarDecl *PVD) -> Param {
                           return Param{.name = PVD->getName().str(),
                                        .shape =
                                            1}; // TODO: support other shapes
                         });
  auto base_body_vec = IR_BuiltStmts.at(FD->getBody());
  assert(base_body_vec.size() == 1);
  // assert body is a BlockStmt and extract the inner block since function
  // bodies don't need the ; at the end of the block statement
  const auto &base_body = base_body_vec.at(0);
  assert(base_body->kind == StmtKind::Block);
  auto body = std::dynamic_pointer_cast<BlockStmt>(base_body)->block;

  auto return_type =
      FD->getReturnType().getAsString(); // TODO handle complicated return types

  IR_FunctionMap.try_emplace(key, location, name, args, return_type, body);
  FunctionOrdering.emplace_back(IR_FunctionMap.at(key));

  return true;
}

auto IRBuilder::TraverseCompoundStmt(clang::CompoundStmt *CS) -> bool {
  if (!RecursiveASTVisitor::TraverseCompoundStmt(CS))
    return false;

  auto *key = CS;
  auto location = CS->getLBracLoc();
  std::vector<StmtPtr> stmts;
  for (auto *stmt : CS->body()) {
    std::println("Processing stmt of type: {}", stmt->getStmtClassName());
    const auto &stmt_vec = IR_BuiltStmts.at(stmt);
    stmts.insert(stmts.end(), stmt_vec.begin(), stmt_vec.end());
  }

  auto block_stmt = std::make_shared<BlockStmt>(
      location, std::make_shared<Block>(location, std::move(stmts)));
  IR_BuiltStmts.try_emplace(key, std::vector<StmtPtr>{block_stmt});

  return true;
}

auto IRBuilder::TraverseDeclStmt(clang::DeclStmt *D) -> bool {
  if (!RecursiveASTVisitor::TraverseDeclStmt(D))
    return false;

  for (auto *decl : D->decls()) {
    if (auto *var_decl = llvm::dyn_cast<clang::VarDecl>(decl)) {
      auto location = var_decl->getLocation();
      auto name = var_decl->getName().str();
      std::optional<int> const shape;

      // TODO support array shapes

      auto init_expr = IR_BuiltExprs.at(var_decl->getInit()->IgnoreImpCasts());
      auto var_decl_stmt = std::make_shared<VarDeclStmt>(
          location, name, init_expr.finalExpr, shape);
      auto &stmt_vec = IR_BuiltStmts[D];
      stmt_vec.insert(stmt_vec.end(), init_expr.preStmts.rbegin(),
                      init_expr.preStmts.rend());
      stmt_vec.push_back(var_decl_stmt);
      stmt_vec.insert(stmt_vec.end(), init_expr.postStmts.begin(),
                      init_expr.postStmts.end());
    } else {
      llvm_unreachable("Only VarDecl is supported under DeclStmt");
    }
  }

  return true;
}

auto IRBuilder::TraverseReturnStmt(clang::ReturnStmt *RS) -> bool {
  if (!RecursiveASTVisitor::TraverseReturnStmt(RS))
    return false;

  auto location = RS->getReturnLoc();
  std::vector<StmtPtr> stmts;
  if (RS->getRetValue() != nullptr) {
    auto ret_built_expr = IR_BuiltExprs.at(RS->getRetValue()->IgnoreImpCasts());
    stmts.insert(stmts.end(), ret_built_expr.preStmts.rbegin(),
                 ret_built_expr.preStmts.rend());
    stmts.push_back(
        std::make_shared<ReturnStmt>(location, ret_built_expr.finalExpr));
    // stmts.insert(stmts.end(), ret_built_expr.postStmts.begin(),
    //              ret_built_expr.postStmts.end());
  } else {
    // return without value, treat it as returning 0
    stmts.push_back(std::make_shared<ExprStmt>(
        location, std::make_shared<IntLitExpr>(location, 0)));
  }

  IR_BuiltStmts.try_emplace(RS, stmts);

  return true;
}

auto IRBuilder::TraverseDeclRefExpr(clang::DeclRefExpr *DRE) -> bool {
  if (!RecursiveASTVisitor::TraverseDeclRefExpr(DRE))
    return false;

  auto location = DRE->getLocation();
  auto name = DRE->getNameInfo().getAsString();

  auto decl_ref_expr = std::make_shared<DeclRefExpr>(location, name);
  IR_BuiltExprs.try_emplace(DRE, BuiltExpression{.finalExpr = decl_ref_expr,
                                                 .preStmts = {},
                                                 .postStmts = {}});

  return true;
}

auto IRBuilder::TraverseIntegerLiteral(clang::IntegerLiteral *IL) -> bool {
  if (!RecursiveASTVisitor::TraverseIntegerLiteral(IL))
    return false;

  auto location = IL->getLocation();
  auto value = IL->getValue().getSExtValue();

  auto int_lit_expr = std::make_shared<IntLitExpr>(location, value);
  IR_BuiltExprs.try_emplace(IL, BuiltExpression{.finalExpr = int_lit_expr,
                                                .preStmts = {},
                                                .postStmts = {}});

  return true;
}