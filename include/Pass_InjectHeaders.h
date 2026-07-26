#ifndef C2PANCAKE_PASS_INJECTHEADERS_H
#define C2PANCAKE_PASS_INJECTHEADERS_H

#include "Pipeline.h"

namespace pancake::pass_inject_headers {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "InjectHeaders"; }
};
} // namespace pancake::pass_inject_headers

#endif // C2PANCAKE_PASS_INJECTHEADERS_H
