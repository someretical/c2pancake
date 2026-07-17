#include "Utils.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/Support/FormatVariadic.h>

#include <string>
#include <utility>
#include <vector>

using namespace pancake;

auto pancake::LogBegin(const PipelineActionCtx &ctx) -> std::string {
  auto action_type = ctx.action_type.value_or(PipelineActionType::None);
  const char *action_type_str{};
  switch (action_type) {
  case PipelineActionType::Rewriter:
    action_type_str = "Rewriter";
    break;
  case PipelineActionType::Analyser:
    action_type_str = "Analyser";
    break;
  default:
    action_type_str = "UnknownActionType";
    break;
  }
  return llvm::formatv("[c2pancake] {0} -> {1}: {2}(i={4:02}) {3}:", ctx.current_file, ctx.next_file, action_type_str,
                       ctx.action_name.value_or("UnknownAction"), ctx.major_pass_number);
}

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
