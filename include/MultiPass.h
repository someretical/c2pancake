#ifndef C2PANCAKE_MULTIPASS_H
#define C2PANCAKE_MULTIPASS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <string>
#include <utility>

namespace pancake {
class MultiPassConsumer : public clang::ASTConsumer {
public:
  explicit MultiPassConsumer(clang::Rewriter &R, std::string file)
      : R(R), file(std::move(file)) {};
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;

private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  clang::Rewriter &R;
  std::string file;
};

class MultiPassAction : public clang::ASTFrontendAction {
public:
  auto CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef file)
      -> std::unique_ptr<clang::ASTConsumer> override;
  void EndSourceFileAction() override;

private:
  clang::Rewriter R;
};

auto runMultiPass(
    const clang::tooling::CompilationDatabase &BaseCompilations,
    const std::vector<std::string> &InputFiles,
    const std::string &CurrentSuffix, // Suffix on the files we are READING
    const std::string &NextSuffix,    // Suffix we append for the next pass
    std::unique_ptr<clang::tooling::FrontendActionFactory> ActionFactory,
    const std::string &PassName) -> std::vector<std::string>;
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
