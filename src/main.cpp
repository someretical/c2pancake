#include "Pass_FunctionCalling.h"
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

#include <cstddef>

using namespace pancake;

namespace {
const char *overview_help = "c2pancake is a tool that converts C code into Pancake code.\n";

llvm::cl::OptionCategory c2_pancake_options("c2pancake options");

const llvm::cl::extrahelp common_help(clang::tooling::CommonOptionsParser::HelpMessage);

const llvm::cl::extrahelp more_help(
    R"(Documentation:
  https://github.com/someretical/c2pancake

Usage examples:
  c2pancake hello_world.c -- -std=c23
)");

void PrintVersion(llvm::raw_ostream &os) {
  os << llvm::formatv("c2pancake 0.0.1\n");
  os << llvm::formatv("LLVM {0}\n", LLVM_VERSION_STRING);
}
} // namespace

llvm::cl::opt<size_t> max_pass_retries( // NOLINT(misc-use-internal-linkage)
    "max-pass-retries",
    llvm::cl::desc("Maximum number of retries for a pass before moving to the next pass (default: 10)"),
    llvm::cl::cat(c2_pancake_options), llvm::cl::init(10));

llvm::cl::opt<std::string> start_at_pass( // NOLINT(misc-use-internal-linkage)
    "start", llvm::cl::desc("Start the pipeline at this pass (default: empty, meaning start at beginning)"),
    llvm::cl::cat(c2_pancake_options), llvm::cl::init(""));

llvm::cl::opt<std::string> end_at_pass( // NOLINT(misc-use-internal-linkage)
    "end",
    llvm::cl::desc(
        "End the pipeline after this pass (including retries) (default: empty, meaning end after last pass)"),
    llvm::cl::cat(c2_pancake_options), llvm::cl::init(""));

auto main(int argc, const char **argv) -> int {
  llvm::cl::SetVersionPrinter(PrintVersion);
  auto parser =
      clang::tooling::CommonOptionsParser::create(argc, argv, c2_pancake_options, llvm::cl::OneOrMore, overview_help);
  if (auto error = parser.takeError()) {
    llvm::errs() << llvm::formatv("[c2pancake] {0}", llvm::fmt_consume(std::move(error)));
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
    }
  };
  */
  Pipeline pipeline(*parser);
  pipeline.AddStage<pass_inject_headers::Action>();
  pipeline.AddStage<pass_name_anon_records::Action>();
  pipeline.AddStage<pass_rename_to_be_promoted_records::Action>();
  pipeline.AddStage<pass_promote_records::Action>();
  pipeline.AddStage<pass_inject_memcpy_polyfill::Action>();
  pipeline.AddStage<pass_function_calling::Action>();
  pipeline.AddStage<pass_normalise_while_loops::Action>();
  pipeline.AddStage<pass_process_continue_in_for_loops::Action>();
  pipeline.AddStage<pass_for_to_while::Action>();
  pipeline.AddStage<pass_process_continue_in_do_while_loops::Action>();
  pipeline.AddStage<pass_do_while_to_while::Action>();
  pipeline.AddStage<pass_add_switch_fallthrough::Action>();
  pipeline.AddStage<pass_normalise_switches::Action>();
  pipeline.AddStage<pass_switch_to_if::Action>();
  pipeline.AddStage<pass_normalise_if_statements::Action>();
  pipeline.AddStage<pass_hoist_condition_expressions::Action>();
  pipeline.AddStage<pass_rewrite_array_indexing::Action>();
  pipeline.AddStage<pass_rewrite_struct_stabs::Action>();
  pipeline.AddStage<pass_lower_nested_expressions::Action>();
  pipeline.AddStage<pass_simplify_double_negation::Action>();
  pipeline.AddStage<pass_lower_bitfield_ops::Action>();
  pipeline.AddStage<pass_simplify_addrof_deref::Action>();
  pipeline.AddStage<pass_implicit_to_explicit_casts::Action>();
  pipeline.AddStage<pass_integer_conversion::Action>();
  // deliberately repeated. Those final explicit casts are just to make the C compiler happy, they have no effect when
  // converting to pancake.
  pipeline.AddStage<pass_implicit_to_explicit_casts::Action>();

  // pipeline.AddPass<pass_hoist_arrays_and_addresses::Action>();
  return pipeline.Run();
}