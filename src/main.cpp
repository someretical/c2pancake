#include "Pass_CompoundAssignment.h"
#include "Pass_HoistArraysAndAddresses.h"
#include "Pass_LoopsToWhile.h"
#include "Pass_PromoteRecords.h"
#include "Pipeline.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

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

  The main thing HandleTranslationUnit should do is add Replacements to pa_ctx.replacements
  The Pipeline will take care of applying the replacements and writing the output file

  class Consumer : public C2PancakePass {
  public:
    using C2PancakePass::C2PancakePass; // inherit constructor
    void HandleTranslationUnit(clang::ASTContext &Ctx) override;
  };

  class Action : public PipelineAction<Consumer> {
  public:
    explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
      ctx.action_name = "<name of pass>";
      // Repeat the pass until no changes are made, and then move onto the next pass
      ctx.failure_behaviour = FailureBehaviour::Repeat;
    }
  };
  */
  Pipeline pipeline(*expected_parser);
  pipeline.AddPass<pass_process_continue_in_for_loops::Action>();
  pipeline.AddPass<pass_for_to_while::Action>();
  pipeline.AddPass<pass_process_continue_in_do_while_loops::Action>();
  pipeline.AddPass<pass_do_while_to_while::Action>();
  // pipeline.AddPass<pass_promote_records::Action>();
  // pipeline.AddPass<pass_hoist_arrays_and_addresses::Action>();
  // pipeline.AddPass<pass_compound_assignment::Action>();
  return pipeline.Run();

  return 0;
}