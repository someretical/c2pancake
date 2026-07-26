#ifndef C2PANCAKE_PASS_NORMALISE_IF_STATEMENTS_H
#define C2PANCAKE_PASS_NORMALISE_IF_STATEMENTS_H

#include "Pipeline.h"

namespace pancake::pass_normalise_if_statements {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "NormaliseIfStatements"; }
};
} // namespace pancake::pass_normalise_if_statements

#endif // C2PANCAKE_PASS_NORMALISE_IF_STATEMENTS