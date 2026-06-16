#ifndef C2PANCAKE_MULTIPASS_H
#define C2PANCAKE_MULTIPASS_H

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/StringRef.h"

#include <memory>

namespace pancake {
using namespace clang;
using namespace clang::tooling;
using namespace llvm;

class MultiPassConsumer : public ASTConsumer {
public:
  explicit MultiPassConsumer(std::shared_ptr<Rewriter> R);
  void HandleTranslationUnit(ASTContext &Ctx) override;

private:
  std::shared_ptr<Rewriter> R;
};

class MultiPassAction : public ASTFrontendAction {
public:
  auto CreateASTConsumer(CompilerInstance &CI, llvm::StringRef file)
      -> std::unique_ptr<ASTConsumer> override;
  void EndSourceFileAction() override;

private:
  llvm::StringRef file;
  std::shared_ptr<Rewriter> R;
};
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
