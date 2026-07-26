#ifndef C2PANCAKE_PASS_LOOPSTOWHILE_H
#define C2PANCAKE_PASS_LOOPSTOWHILE_H

#include "Pipeline.h"

namespace pancake::pass_normalise_while_loops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "NormaliseWhileLoops"; }
};
} // namespace pancake::pass_normalise_while_loops

namespace pancake::pass_process_continue_in_for_loops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) {
    ctx.action_name = "RewriteContinueInForLoops";
  }
};
} // namespace pancake::pass_process_continue_in_for_loops

namespace pancake::pass_for_to_while {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "RewriteForToWhile"; }
};
} // namespace pancake::pass_for_to_while

namespace pancake::pass_process_continue_in_do_while_loops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) {
    ctx.action_name = "RewriteContinueInDoWhileLoops";
  }
};
} // namespace pancake::pass_process_continue_in_do_while_loops

namespace pancake::pass_do_while_to_while {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineStage<Consumer> {
public:
  explicit Action(PipelineStageCtx &ctx) : PipelineStage<Consumer>(ctx) { ctx.action_name = "RewriteDoWhileToWhile"; }
};
} // namespace pancake::pass_do_while_to_while
#endif // C2PANCAKE_PASS_LOOPSTOWHILE_H