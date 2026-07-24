#include "Pass_InjectHeaders.h"
#include "Pass_IntegerConversion.h"
#include "Pass_LoopsToWhile.h"
#include "Pass_LowerBitfieldOps.h"
#include "Pass_NormaliseIfStatements.h"
#include "Pass_PromoteRecords.h"
#include "Pass_SwitchToIf.h"
#include "Pass_TransformLogicalExpressions.h"
#include "Pipeline.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
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
    llvm::errs() << llvm::formatv("[c2pancake] {0}", llvm::fmt_consume(expected_parser.takeError()));
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
  pipeline.AddPass<pass_inject_headers::Action>();
  pipeline.AddPass<pass_name_anon_records::Action>();
  pipeline.AddPass<pass_rename_to_be_promoted_records::Action>();
  pipeline.AddPass<pass_normalise_while_loops::Action>();
  pipeline.AddPass<pass_process_continue_in_for_loops::Action>();
  pipeline.AddPass<pass_for_to_while::Action>();
  pipeline.AddPass<pass_process_continue_in_do_while_loops::Action>();
  pipeline.AddPass<pass_do_while_to_while::Action>();
  pipeline.AddPass<pass_add_switch_fallthrough::Action>();
  pipeline.AddPass<pass_normalise_switches::Action>();
  pipeline.AddPass<pass_switch_to_if::Action>();
  pipeline.AddPass<pass_normalise_if_statements::Action>();
  pipeline.AddPass<pass_hoist_condition_expressions::Action>();
  pipeline.AddPass<pass_rewrite_array_indexing::Action>();
  pipeline.AddPass<pass_rewrite_struct_stabs::Action>();
  pipeline.AddPass<pass_lower_nested_expressions::Action>();
  pipeline.AddPass<pass_lower_bitfield_ops::Action>();
  pipeline.AddPass<pass_simplify_addrof_deref::Action>();
  pipeline.AddPass<pass_promote_records::Action>();
  pipeline.AddPass<pass_implicit_to_explicit_casts::Action>();
  pipeline.AddPass<pass_integer_conversion::Action>();
  // deliberately repeated. Those final explicit casts are just to make the C compiler happy, they have no effect when
  // converting to pancake.
  pipeline.AddPass<pass_implicit_to_explicit_casts::Action>();

  // pipeline.AddPass<pass_hoist_arrays_and_addresses::Action>();
  return pipeline.Run();

  return 0;
}