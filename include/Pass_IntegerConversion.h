#ifndef C2PANCAKE_PASS_INTEGERCONVERSION_H
#define C2PANCAKE_PASS_INTEGERCONVERSION_H

#include "Pipeline.h"

namespace pancake::pass_implicit_to_explicit_casts {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "ImplicitToExplicitCasts"; }
};
} // namespace pancake::pass_implicit_to_explicit_casts

namespace pancake::pass_integer_conversion {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "ConvertIntegerTypes"; }
};
} // namespace pancake::pass_integer_conversion

#endif // C2PANCAKE_PASS_INTEGERCONVERSION_H
