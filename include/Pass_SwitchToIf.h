#ifndef C2PANCAKE_PASS_PASS_ADD_SWITCH_FALLTHROUGH_H
#define C2PANCAKE_PASS_PASS_ADD_SWITCH_FALLTHROUGH_H

#include "Pipeline.h"

namespace pancake::pass_add_switch_fallthrough {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "AddSwitchFallthroughs"; }
};
} // namespace pancake::pass_add_switch_fallthrough

namespace pancake::pass_normalise_switches {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "NormaliseSwitches"; }
};
} // namespace pancake::pass_normalise_switches

namespace pancake::pass_switch_to_if {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) {
    ctx.stage_name = "RewriteSwitchesToIfStatements";
  }
};
} // namespace pancake::pass_switch_to_if
#endif // C2PANCAKE_PASS_PASS_ADD_SWITCH_FALLTHROUGH_H