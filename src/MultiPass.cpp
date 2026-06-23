#include "MultiPass.h"

#include <llvm/Support/ErrorHandling.h>

using namespace pancake;

auto Pipeline::Run() -> int {
  const std::vector<std::string> initial_files = options_parser.getSourcePathList();

  for (const auto &initial_file : initial_files) {
    std::string current_suffix{};
    for (size_t j = 0; j < factories.size(); ++j) {
      auto &factory = factories.at(j);
      for (size_t k = 0; k < 4; ++k) {
        auto next_suffix = std::format(".c2pnk.{}-{}.c", j, k);
        StagedCompilationDatabase db(options_parser.getCompilations(), current_suffix);
        auto current_file = std::format("{}{}", initial_file, current_suffix);
        clang::tooling::ClangTool tool(db, std::vector{current_file});

        PipelineActionCtx ctx(k, current_suffix, next_suffix);
        auto action = factory->BetterCreate(ctx);
        if (ctx.failure_behaviour == FailureBehaviour::NONE) {
          llvm_unreachable("Action did not specify a failure behaviour");
        }
        if (ctx.action_name.empty()) {
          llvm_unreachable("Action did not specify an action name");
        }

        auto commands = db.getCompileCommands(current_file);
        if (commands.size() > 1) {
          llvm::outs() << llvm::formatv("Warning: {0} compile commands found for {1}\n", commands.size(), current_file);
        }
        for (auto &cmd : commands) {
          clang::tooling::ToolInvocation invocation(cmd.CommandLine, std::move(action), &tool.getFiles());
          if (!invocation.run()) {
            // abnormal failure
            llvm::errs() << "Error: " << ctx.action_name << " failed.\n";
            return 1;
          };
        }

        llvm::outs() << llvm::formatv("c2pancake: Pass {0}-{1} completed\n", ctx.action_name, k);

        current_suffix = next_suffix;

        if (ctx.failure_behaviour == FailureBehaviour::Repeat && ctx.failure_mode == FailureMode::Repeat) {
          // run pass again
        } else {
          // move onto next pass
          break;
        }
      }
    }
  }

  llvm::outs() << "c2pancake: All passes completed successfully.\n";
  return 0;
}