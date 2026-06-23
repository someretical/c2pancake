#ifndef C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H
#define C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H

#include "MultiPass.h"

namespace pancake::pass_compound_assignment {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "ExpandCompoundAssignments";
    ctx.failure_behaviour = FailureBehaviour::Repeat;
  }
};
} // namespace pancake::pass_compound_assignment

#endif // C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H