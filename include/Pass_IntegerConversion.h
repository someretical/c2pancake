#ifndef C2PANCAKE_PASS_INTEGERCONVERSION_H
#define C2PANCAKE_PASS_INTEGERCONVERSION_H

#include "Pipeline.h"

namespace pancake::pass_implicit_to_explicit_casts {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "ImplicitToExplicitCasts";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_implicit_to_explicit_casts

namespace pancake::pass_integer_conversion {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "ConvertIntegerTypes";
    ctx.failure_behaviour = FailureBehaviour::MoveToNextFile;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_integer_conversion

#endif // C2PANCAKE_PASS_INTEGERCONVERSION_H