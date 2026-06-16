#include "MultiPass.h"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

auto main(int argc, const char **argv) -> int {
  llvm::cl::OptionCategory c2_pancake_options("c2pancake options");
  const cl::extrahelp common_help(CommonOptionsParser::HelpMessage);
  const cl::extrahelp more_help("\nMore help text...\n");

  auto expected_parser =
      CommonOptionsParser::create(argc, argv, c2_pancake_options);
  if (!expected_parser) {
    llvm::errs() << expected_parser.takeError();
    return 1;
  }
  CommonOptionsParser &options_parser = expected_parser.get();

  ClangTool tool(options_parser.getCompilations(),
                 options_parser.getSourcePathList());
  return tool.run(newFrontendActionFactory<pancake::MultiPassAction>().get());
}
