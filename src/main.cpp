#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

class MultiPassConsumer : public ASTConsumer {
public:
  explicit MultiPassConsumer(Rewriter &R) : R(R) {}

  void HandleTranslationUnit(ASTContext &Ctx) override {
    auto TU = Ctx.getTranslationUnitDecl();
    for (const auto D_ : TU->decls()) {
      if (const auto *D = llvm::dyn_cast<FunctionDecl>(D_)) {
        // for (const auto *P : D->parameters()) {
        //   // handle
        // }
        // if (const Stmt *Body = D->getBody()) {
        //   // traversePostOrder(Body);
        // }
      } /* TODO more cases*/
    }
  }

private:
  Rewriter &R;

  void traversePostOrder(const Stmt *S) {

    // TODO do something with S
  };
}

class MultiPassAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &CI, llvm::StringRef file) override {
    R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<MultiPassConsumer>(R);
  }

  void EndSourceFileAction() override {
    // write output to stdout instead of overwriting the original file
    R.getEditBuffer(R.getSourceMgr().getMainFileID()).write(llvm::outs());
  }

private:
  Rewriter R;
};

static llvm::cl::OptionCategory C2PancakeOptions("c2pancake options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nMore help text...\n");

int main(int argc, const char **argv) {
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, C2PancakeOptions);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();

  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<MultiPassAction>().get());
}
