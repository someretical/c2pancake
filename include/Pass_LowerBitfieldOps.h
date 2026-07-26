#ifndef C2PANCAKE_PASS_LOWERBITFIELDOPS_H
#define C2PANCAKE_PASS_LOWERBITFIELDOPS_H

#include "Pipeline.h"

namespace pancake::pass_lower_bitfield_ops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "LowerBitfieldOps"; }
};
} // namespace pancake::pass_lower_bitfield_ops

namespace pancake::pass_simplify_addrof_deref {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) {
    ctx.action_name = "SimplifyAddressOfFollowedByDereference";
  }
};
} // namespace pancake::pass_simplify_addrof_deref

#endif // C2PANCAKE_PASS_LOWERBITFIELDOPS_H