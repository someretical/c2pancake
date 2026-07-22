#ifndef C2PANCAKE_PASS_LOWERBITFIELDOPS_H
#define C2PANCAKE_PASS_LOWERBITFIELDOPS_H

#include "Pipeline.h"

namespace pancake::pass_lower_bitfield_ops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "LowerBitfieldOps";
    ctx.failure_behaviour = FailureBehaviour::MoveToNextPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_lower_bitfield_ops

namespace pancake::pass_simplify_addrof_deref {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "SimplifyAddressOfFollowedByDereference";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_simplify_addrof_deref

#endif // C2PANCAKE_PASS_LOWERBITFIELDOPS_H