#ifndef C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H
#define C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H

#include "Pipeline.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace pancake::pass_hoist_arrays_and_addresses {

enum class DeclTreatment : uint8_t {
  // Auto/register local: emit a fresh global decl before the first function,
  // replace the local DeclStmt with initialisation assignments
  NewGlobal,

  // Static local: strip "static", move the whole declaration (with any
  // constant initialiser) to the global block, remove the original DeclStmt
  StaticGlobal,

  // Extern local: the symbol already exists externally; just delete the
  // local re-declaration and rename uses to the bare external name
  ExternRedecl,
};

struct VarHoistEntry {
  std::string newName;
  DeclTreatment treatment;
  const clang::FunctionDecl *func{};

  VarHoistEntry(std::string newName, DeclTreatment treatment, const clang::FunctionDecl *func)
      : newName(std::move(newName)), treatment(treatment), func(func) {}
};

struct HoistInfo {
  llvm::DenseMap<const clang::VarDecl *, VarHoistEntry> hoistMap;
  size_t hoist_arr_counter = 0;
  size_t hoist_ptr_counter = 0;
  size_t hoist_record_counter = 0;
};

// Collect all variables that need hoisting
class PassFind : public clang::RecursiveASTVisitor<PassFind> {
public:
  std::set<const clang::VarDecl *> addressTaken;
  std::set<const clang::VarDecl *> arrayFieldAccessed;

  auto VisitUnaryOperator(clang::UnaryOperator *UO) -> bool;
  auto VisitMemberExpr(clang::MemberExpr *ME) -> bool;
};

// Analyse how each variable should be hoisted and prepare the replacement map
class PassAnalyse : public clang::RecursiveASTVisitor<PassAnalyse> {
public:
  HoistInfo &info;
  clang::ASTContext &Ctx;

  PassAnalyse(HoistInfo &hi, clang::ASTContext &ctx) : info(hi), Ctx(ctx) {}

  auto VisitFunctionDecl(clang::FunctionDecl *FD) -> bool;

private:
  void WalkStmt(clang::Stmt *S, clang::FunctionDecl *FD, PassFind &atf);
};

// Accumulate replacements for DeclStmts and DeclRefExprs
class PassRename : public clang::RecursiveASTVisitor<PassRename> {
public:
  clang::tooling::Replacements &Repls;
  clang::ASTContext &Ctx;
  HoistInfo &info;
  clang::SourceManager &SM;
  bool has_replacement_error = false;

  PassRename(clang::tooling::Replacements &repls, clang::ASTContext &ctx, HoistInfo &hi)
      : Repls(repls), Ctx(ctx), info(hi), SM(ctx.getSourceManager()) {}

  // Replace DeclStmts that contain hoisted vars
  auto VisitDeclStmt(clang::DeclStmt *DS) -> bool;

  // Rename every use of a hoisted variable
  auto VisitDeclRefExpr(clang::DeclRefExpr *DR) -> bool;

private:
  // Convert a SourceRange to a tooling::Replacement and add it to the set
  void AddReplacement(clang::SourceRange range, llvm::StringRef text);
};

class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "HoistArraysAndAddresses";
    ctx.failure_behaviour = FailureBehaviour::Continue;
  }
};

} // namespace pancake::pass_hoist_arrays_and_addresses

#endif // C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H