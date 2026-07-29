#ifndef C2PANCAKE_PASS_FUNCTIONCALLING_H
#define C2PANCAKE_PASS_FUNCTIONCALLING_H

#include "Pipeline.h"

namespace pancake::pass_inject_memcpy_polyfill {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "InjectMemcpyPolyfill"; }
};
} // namespace pancake::pass_inject_memcpy_polyfill

namespace pancake::pass_function_calling {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "RewriteFunctionCalling"; }
};
} // namespace pancake::pass_function_calling

#endif // C2PANCAKE_PASS_FUNCTIONCALLING_H