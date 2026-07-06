#include "Pass_CompoundAssignment.h"
#include "Pass_HoistArraysAndAddresses.h"
#include "Pass_LoopsToWhile.h"
#include "Pass_NormaliseIfStatements.h"
#include "Pass_PromoteRecords.h"
#include "Pass_SwitchToIf.h"
#include "Pass_TransformLogicalExpressions.h"
#include "Pipeline.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
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
  pipeline.AddPass<pass_process_continue_in_for_loops::Action>();
  pipeline.AddPass<pass_for_to_while::Action>();
  pipeline.AddPass<pass_process_continue_in_do_while_loops::Action>();
  pipeline.AddPass<pass_do_while_to_while::Action>();
  pipeline.AddPass<pass_add_switch_fallthrough::Action>();
  pipeline.AddPass<pass_normalise_switches::Action>();
  pipeline.AddPass<pass_switch_to_if::Action>();
  pipeline.AddPass<pass_normalise_if_statements::Action>();
  pipeline.AddPass<pass_hoist_condition_expressions::Action>();
  pipeline.AddPass<pass_lower_nested_expressions::Action>();
  // pipeline.AddPass<pass_promote_records::Action>();
  // pipeline.AddPass<pass_hoist_arrays_and_addresses::Action>();
  // pipeline.AddPass<pass_compound_assignment::Action>();
  return pipeline.Run();

  // hoist if-statement conditions with side effects to temporary variables
  // hoist while-statement conditions with side effects to temporary variables
  // hoist return statements with side effects to temporary variables

  // logical operators can still occur in:
  // assignment expressions, e.g. a = b && c;
  // function call arguments, e.g. f(a && b);
  // comma expressions
  // array subscripts
  // operands of other operators, e.g. a + (b && c)

  // loop the following steps until no changes across all steps:
  // - hoist array indices with side effects to temporary variable
  // this can ONLY happen if the indexing happens at the top level of an assignment expression, e.g. a[b++] = c;
  // - hoist array assignment ops RHS with side effects to temporary variables
  // this can ONLY happen if the assignment happens at the top level of an assignment expression, e.g. a[b] = c++ + d;
  // - hoist assignment ops RHS with side effects to temporary variables
  // this can ONLY happen if the assignment happens at the top level of an assignment expression, e.g. a = b++ + c;
  // - hoist function call arguments with side effects to temporary variables
  // this can ONLY happen if the function call is at the top level of an assignment expression, e.g. f(a++, b++);
  // - expand comma expressions into multiple statements
  // this can ONLY happen if the comma expression is at the top level of an assignment expression, e.g. a = (b++, c++);
  // - expand compound assignment operators into simple assignment operators with temp vars
  // this can ONLY happen if the compound assignment is at the top level of an assignment expression, e.g. a += b;
  // - expand logical operators into if statements with temp vars
  // this can ONLY happen if the logical operator is at the top level of an assignment expression, e.g. a = b && c;

  return 0;
}