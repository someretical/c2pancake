#include "Pipeline.h"

#include <llvm/Support/ErrorHandling.h>

using namespace pancake;

auto pancake::StagedCompilationDatabase::GetOriginalFilename(llvm::StringRef Filename) const -> llvm::StringRef {
  const auto ref = Filename;
  if (!current_suffix.empty() && ref.ends_with(current_suffix)) {
    return ref.drop_back(current_suffix.size());
  }
  return ref;
}

auto pancake::StagedCompilationDatabase::getCompileCommands(llvm::StringRef Filename) const
    -> std::vector<clang::tooling::CompileCommand> {
  auto original_file = GetOriginalFilename(Filename);
  auto commands = base_db.getCompileCommands(original_file);
  std::vector<clang::tooling::CompileCommand> ret_commands;

  for (auto &command : commands) {
    bool found = false;
    if (command.Filename == original_file) {
      command.Filename = Filename;
      found = true;
    }

    for (auto &arg : command.CommandLine) {
      if (arg == original_file) {
        arg = Filename;
      }
    }

    if (found) {
      ret_commands.push_back(std::move(command));
    }
  }

  return ret_commands;
}

auto Pipeline::Run() -> int {
  const std::vector<std::string> initial_files = options_parser.getSourcePathList();

  for (const auto &initial_file : initial_files) {
    std::string current_suffix{};
    for (size_t j = 0; j < factories.size(); ++j) {
      auto &factory = factories.at(j);
      for (size_t k = 0; k < 4; ++k) {
        auto next_suffix = llvm::formatv(".{0}-{1}-c2pnk.c", j, k);
        StagedCompilationDatabase db(options_parser.getCompilations(), current_suffix);
        auto current_file = llvm::formatv("{0}{1}", initial_file, current_suffix);
        clang::tooling::ClangTool tool(db, std::vector{current_file.str()});

        PipelineActionCtx ctx(k, current_suffix, next_suffix);
        auto action = factory->BetterCreate(ctx);
        if (ctx.failure_behaviour == FailureBehaviour::NONE) {
          llvm_unreachable("Action did not specify a failure behaviour");
        }
        if (ctx.action_name.empty()) {
          llvm_unreachable("Action did not specify an action name");
        }

        auto commands = db.getCompileCommands(current_file.str());
        if (commands.size() > 1) {
          llvm::outs() << llvm::formatv("{0} WARNING: {1} (>1) compile commands found\n", LogBeginShort(current_file),
                                        commands.size());
        }
        for (auto &cmd : commands) {
          clang::tooling::ToolInvocation invocation(cmd.CommandLine, std::move(action), &tool.getFiles());
          if (!invocation.run()) {
            // abnormal failure
            llvm::errs() << llvm::formatv("{0} ERROR: {1} failed.\n", LogBeginShort(current_file), ctx.action_name);
            return 1;
          };
        }

        llvm::outs() << llvm::formatv("{0} {1} changes applied\n", LogBegin(ctx, current_file),
                                      ctx.replacements.size());

        if (ctx.failure_behaviour == FailureBehaviour::Repeat && ctx.failure_mode == FailureMode::Repeat) {
          current_suffix = next_suffix;
          // run pass again
        } else {
          llvm::outs() << llvm::formatv("{0} Pass {1} complete after {2} iteration(s)\n", LogBeginShort(current_file),
                                        ctx.action_name, k + 1);
          current_suffix = next_suffix;
          break;
        }
      }
    }
  }

  llvm::outs() << "[c2pancake] All passes completed successfully\n";
  return 0;
}
