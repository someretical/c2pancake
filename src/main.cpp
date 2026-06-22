#include "MultiPass.h"
#include "Pass_HoistArraysAndAddresses.h"
#include "Pass_PromoteRecords.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <vector>

using namespace pancake;

namespace {
llvm::cl::OptionCategory c2_pancake_options("c2pancake options");
const llvm::cl::extrahelp common_help(clang::tooling::CommonOptionsParser::HelpMessage);
const llvm::cl::extrahelp more_help("\nMore help text...\n");
} // namespace

auto main(int argc, const char **argv) -> int {

  auto expected_parser = clang::tooling::CommonOptionsParser::create(argc, argv, c2_pancake_options);
  if (!expected_parser) {
    llvm::errs() << expected_parser.takeError();
    return 1;
  }

  /*
  To add a new stage to the pipeline, use the following code.
  Consumer::HandleTranslationUnit is the entry point and must be implemented.

  You should queue all replacements to the reference `repls` in the Consumer and they will be applied
  automatically in PipelineAction::EndSourceFileAction after HandleTranslationUnit returns.
  PipelineAction::EndSourceFileAction also handles writing to the output file.

  class Consumer : public C2PancakePass {
  public:
    using C2PancakePass::C2PancakePass; // inherit constructor
    void HandleTranslationUnit(clang::ASTContext &Ctx) override;
  };

  class Action : public PipelineAction<Action, Consumer> {
  public:
    using PipelineAction<Action, Consumer>::PipelineAction; // inherit constructor
    static auto GetActionName() -> std::string { return "YOUR_PASS_NAME_HERE"; }
  };
  */
  Pipeline pipeline(*expected_parser);
  pipeline.AddPass<pass_promote_records::Action>();
  pipeline.AddPass<pass_hoist_arrays_and_addresses::Action>();
  return pipeline.Run();

  return 0;
}