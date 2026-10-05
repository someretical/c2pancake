#ifndef C2PANCAKE_PASS_C2PANCAKE_H
#define C2PANCAKE_PASS_C2PANCAKE_H

#include "Pipeline.h"

namespace pancake::pass_c2pancake {
struct Finalizer {
  static auto Write(StageContext &ctx, clang::CompilerInstance &compiler) -> llvm::Error;
};

class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer>(ctx) { ctx.stage_name = "C2Pancake"; }
};
} // namespace pancake::pass_c2pancake

#endif // C2PANCAKE_PASS_C2PANCAKE_H
