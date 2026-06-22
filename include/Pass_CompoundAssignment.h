#ifndef C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H
#define C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H

#include "MultiPass.h"

namespace pancake::pass_compound_assignment {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Action, Consumer> {
public:
  using PipelineAction<Action, Consumer>::PipelineAction; // inherit constructor
  static auto GetActionName() -> std::string { return "Pass_CompoundAssignment"; }
};
} // namespace pancake::pass_compound_assignment

#endif // C2PANCAKE_PASS_COMPOUNDASSIGNMENT_H