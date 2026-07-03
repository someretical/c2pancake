#ifndef C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS_H
#define C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS_H

#include "Pipeline.h"

/*
lift the condition expressions out of if statements and hoist them to temporary variables, so that the if statement
conditions are pure variable references. same thing applied to while loops and return statements
*/
namespace pancake::pass_hoist_condition_expressions {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "HoistLogicalSideEffectExpressions";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_hoist_condition_expressions

/*
turn all logical OR and ANDs into if-else statements if they have side effects
*/
namespace pancake::pass_transform_logical_expressions {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "TransformLogicalExpressions";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_transform_logical_expressions

/*
Turn all nested expressions with side effects into temporary variables, so that all expressions are pure variable
references
*/
namespace pancake::pass_lower_nested_expressions {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "LowerNestedExpressions";
    ctx.failure_behaviour = FailureBehaviour::MoveToNextPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_lower_nested_expressions

#endif // C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS