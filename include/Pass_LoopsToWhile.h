#ifndef C2PANCAKE_PASS_LOOPSTOWHILE_H
#define C2PANCAKE_PASS_LOOPSTOWHILE_H

#include "Pipeline.h"

namespace pancake::pass_process_continue_in_for_loops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteContinueInForLoops";
    ctx.failure_behaviour = FailureBehaviour::Continue;
  }
};
} // namespace pancake::pass_process_continue_in_for_loops

namespace pancake::pass_for_to_while {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteForToWhile";
    ctx.failure_behaviour = FailureBehaviour::Repeat;
  }
};
} // namespace pancake::pass_for_to_while

namespace pancake::pass_process_continue_in_do_while_loops {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteContinueInDoWhileLoops";
    ctx.failure_behaviour = FailureBehaviour::Continue;
  }
};
} // namespace pancake::pass_process_continue_in_do_while_loops

namespace pancake::pass_do_while_to_while {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteDoWhileToWhile";
    ctx.failure_behaviour = FailureBehaviour::Repeat;
  }
};
} // namespace pancake::pass_do_while_to_while
#endif // C2PANCAKE_PASS_LOOPSTOWHILE_H