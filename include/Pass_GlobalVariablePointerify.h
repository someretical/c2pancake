#ifndef C2PANCAKE_PASS_GLOBALVARIABLEPOINTERIFY_H
#define C2PANCAKE_PASS_GLOBALVARIABLEPOINTERIFY_H

#include "Pipeline.h"

namespace pancake::pass_global_variable_pointerify {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "GlobalVariablePointerify"; }
};
} // namespace pancake::pass_global_variable_pointerify
#endif // C2PANCAKE_PASS_GLOBALVARIABLEPOINTERIFY_H