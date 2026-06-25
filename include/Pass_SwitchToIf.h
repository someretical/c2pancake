#ifndef C2PANCAKE_PASS_NORMALISE_SWITCHES_H
#define C2PANCAKE_PASS_NORMALISE_SWITCHES_H

#include "Pipeline.h"

namespace pancake::normalise_switches {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "NormaliseSwitches";
    ctx.failure_behaviour = FailureBehaviour::MoveToNextFile;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::normalise_switches

namespace pancake::pass_switch_to_if {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteSwitchToIf";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_switch_to_if
#endif // C2PANCAKE_PASS_NORMALISE_SWITCHES_H