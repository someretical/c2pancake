#include "MultiPass.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"

#include <llvm/ADT/StringRef.h>
#include <memory>
#include <utility>

using namespace pancake;
using namespace clang;
using namespace clang::tooling;
using namespace llvm;

MultiPassConsumer::MultiPassConsumer(std::shared_ptr<Rewriter> R)
    : R(std::move(std::move(R))) {}

auto MultiPassConsumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  // transform function calls which take stack pointers
  // build IR
}

auto MultiPassAction::CreateASTConsumer(CompilerInstance &CI,
                                        llvm::StringRef file)
    -> std::unique_ptr<ASTConsumer> {
  this->file = file;
  R->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return std::make_unique<MultiPassConsumer>(R);
}

auto MultiPassAction::EndSourceFileAction() -> void {
  // Write the accumulated rewritten buffer to a new file in the same location
  // as the old but with a .sml extension
  // auto outputFile = std::format("{}", std::string(file.str()) + ".sml");
  // std::error_code EC;
  // llvm::raw_fd_ostream out(outputFile, EC, llvm::sys::fs::OF_None);
  // if (EC) {
  //   llvm::errs() << "Could not open file: " << EC.message() << "\n";
  //   return;
  // }
  // R.getEditBuffer(R.getSourceMgr().getMainFileID()).write(out);
}
