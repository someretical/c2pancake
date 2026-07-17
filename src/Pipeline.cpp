#include "Pipeline.h"
#include "Utils.h"

#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace pancake;

auto Pipeline::Run() -> int {
  const std::vector<std::string> initial_files = options_parser.getSourcePathList();

  for (const auto &initial_file : initial_files) {
    std::string current_suffix{};
    bool move_onto_next_file = false;

    llvm::outs() << llvm::formatv("{0} Starting processing\n", LogBeginShort(initial_file));

    for (size_t j = 0; j < factories.size(); ++j) {
      auto &factory = factories.at(j);
      for (size_t k = 0; k < 10; ++k) {
        std::string next_suffix = llvm::formatv(".{0}-{1}-c2pnk.c", j, k);
        StagedCompilationDatabase const db(options_parser.getCompilations(), current_suffix);
        std::string const current_file = llvm::formatv("{0}{1}", initial_file, current_suffix);
        std::string const next_file = llvm::formatv("{0}{1}", initial_file, next_suffix);
        clang::tooling::ClangTool tool(db, std::vector{current_file});

        clang::tooling::Replacements replacements;
        PipelineActionCtx ctx(j, k, replacements, current_file, current_suffix, next_file, next_suffix);
        auto action = factory->BetterCreate(ctx);
        if (!ctx.failure_behaviour.has_value()) {
          llvm_unreachable("Action did not set a failure behaviour");
        }
        if (!ctx.action_name.has_value()) {
          llvm_unreachable("Action did not set an action name");
        }
        if (!ctx.action_type.has_value()) {
          llvm_unreachable("Action did not set an action type");
        }

        auto commands = db.getCompileCommands(current_file);
        if (commands.size() > 1) {
          llvm::outs() << llvm::formatv("{0} WARNING: {1} (>1) compile commands found\n", LogBegin(ctx),
                                        commands.size());
        }
        for (auto &cmd : commands) {
          clang::tooling::ToolInvocation invocation(cmd.CommandLine, std::move(action), &tool.getFiles());
          if (!invocation.run()) {
            // abnormal failure
            llvm::errs() << llvm::formatv("{0} ERROR: {1} failed.\n", LogBegin(ctx), ctx.action_name.value());
            return 1;
          };
        }

        llvm::outs() << llvm::formatv("{0} {1} change(s) applied\n", LogBegin(ctx), ctx.replacements.size());

        auto action_type = ctx.action_type.value();
        if (action_type == PipelineActionType::Rewriter) {
          if (ctx.failure_mode == FailureMode::Success) {
            llvm::outs() << llvm::formatv("{0} Complete after {2} iteration(s)\n", LogBegin(ctx),
                                          ctx.action_name.value(), k + 1);
            current_suffix = next_suffix;
            break;
          }

          if (ctx.failure_mode == FailureMode::Fail) {
            llvm::errs() << llvm::formatv("{0} Failed after {2} iteration(s)\n", LogBegin(ctx), ctx.action_name.value(),
                                          k + 1);
            move_onto_next_file = true;
            break;
          }

          if (ctx.failure_behaviour == FailureBehaviour::RepeatPass && ctx.failure_mode == FailureMode::RepeatPass) {
            current_suffix = next_suffix;
            // run pass again
          } else {

            llvm::outs() << llvm::formatv("{0} Unknown state, ctx.failure_behaviour: {1}, ctx.failure_mode: {2}\n",
                                          LogBegin(ctx), std::to_underlying(ctx.failure_behaviour.value()),
                                          std::to_underlying(ctx.failure_mode));
            llvm_unreachable("");
          }
        } else if (action_type == PipelineActionType::Analyser) {
          if (ctx.failure_mode == FailureMode::Success) {
            llvm::outs() << llvm::formatv("{0} Analysis successful\n", LogBegin(ctx), ctx.action_name.value());
            break;
          }

          if (ctx.failure_mode == FailureMode::Fail) {
            llvm::errs() << llvm::formatv("{0} Analysis failed\n", LogBegin(ctx), ctx.action_name.value());
            move_onto_next_file = true;
            break;
          }

          llvm_unreachable("Analyser passes should not set failure_mode to RepeatPass");
        } else {
          llvm_unreachable("Unknown PipelineActionType");
        }
      }

      if (move_onto_next_file) {
        break;
      }
    }

    llvm::outs() << llvm::formatv("{0} All passes complete, output written to {1}{2}\n", LogBeginShort(initial_file),
                                  initial_file, current_suffix);
  }

  llvm::outs() << "[c2pancake] All passes completed successfully\n";
  return 0;
}
