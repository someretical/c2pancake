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

namespace pancake::pass_rename_to_be_hoisted_globals {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "RenameToBeHoistedLocals"; }
};
} // namespace pancake::pass_rename_to_be_hoisted_globals

namespace pancake::pass_hoist_locals {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "HoistLocals"; }
};
} // namespace pancake::pass_hoist_locals

#endif // C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H