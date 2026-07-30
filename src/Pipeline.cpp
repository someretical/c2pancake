#include "Pipeline.h"
#include "Utils.h"

#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace pancake;

extern llvm::cl::opt<size_t> max_pass_retries;
extern llvm::cl::opt<std::string> start_at_pass;
extern llvm::cl::opt<std::string> end_at_pass;

auto Pipeline::Run() -> int {
  const auto &initial_files = options_parser.getSourcePathList();

  for (const auto &initial_file : initial_files) {
    std::string current_suffix{};

    PrintLogBeginShort(llvm::outs(), initial_file);
    llvm::outs() << "Starting processing\n";
    bool start_at_pass_found = start_at_pass.empty();

    for (size_t pass_num = 0; pass_num < factories.size(); ++pass_num) {
      auto &factory = factories.at(pass_num);
      for (size_t pass_retry = 0; pass_retry < max_pass_retries; ++pass_retry) {
        std::string next_suffix = llvm::formatv(".{0}-{1}-c2pnk.c", pass_num, pass_retry);
        const StagedCompilationDatabase db(options_parser.getCompilations(), current_suffix);
        const std::string current_file = llvm::formatv("{0}{1}", initial_file, current_suffix);
        const std::string next_file = llvm::formatv("{0}{1}", initial_file, next_suffix);
        clang::tooling::ClangTool tool(db, llvm::SmallVector<std::string, 1>{current_file});

        auto commands = db.getCompileCommands(current_file);
        if (commands.size() > 1) {
          PrintLogBeginShort(llvm::errs(), current_file);
          llvm::errs() << llvm::formatv("WARNING: {0} (>1) compile commands found, only executing the first one\n",
                                        commands.size());
          for (const auto [i, cmd] : commands | std::views::enumerate) {
            llvm::errs() << llvm::formatv("    {0}: {1}\n", i, llvm::join(cmd.CommandLine, " "));
          }
        }

        const auto &command = commands.at(0);
        clang::tooling::Replacements replacements;
        PipelineStageCtx ctx(pass_num, pass_retry, replacements, current_file, current_suffix, next_file, next_suffix);
        auto action = factory->BetterCreate(ctx);
        if (!ctx.action_name.has_value()) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Action did not set an action name\n");
          return 1;
        }

        clang::tooling::ToolInvocation invocation(command.CommandLine, std::move(action), &tool.getFiles());

        if (!start_at_pass_found) {
          if (*ctx.action_name == start_at_pass) {
            start_at_pass_found = true;
          } else {
            PrintLogBegin(llvm::outs(), ctx);
            llvm::outs() << "Skipping pass because name didn't match \"start\" option\n";
            goto move_to_next_pass;
          }
        }

        if (!invocation.run()) {
          // abnormal failure
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: ToolInvocation.run() failed\n");
          return 1;
        };

        if (!ctx.whats_next.has_value()) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Action did not set what's next\n");
          return 1;
        }

        if (ctx.end_src_file_action_error) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Tool invocation {0} failed: {1}\n", ctx.action_name.value(),
                                        ctx.end_src_file_action_error);
          return 1;
        }

        if (ctx.error) {
          // runtime error
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << ctx.error;
          return 1;
        }

        switch (ctx.whats_next.value()) {
        case WhatsNext::MoveToNextFile: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Move to next file requested\n");
          goto move_to_next_file;
        }
        case WhatsNext::MoveToNextPass: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Move to next pass requested\n");
          if (ctx.file_modified) {
            current_suffix = next_suffix;
          }
          goto move_to_next_pass;
        }
        case WhatsNext::RepeatPass: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Repeat pass requested\n");

          if (!ctx.file_modified) {
            PrintLogBegin(llvm::errs(), ctx);
            llvm::errs() << llvm::formatv("FATAL: Repeat pass requested but no modifications were made to the source "
                                          "file, this will result in an infinite loop\n");
            return 1;
          }

          current_suffix = next_suffix;

          if (pass_retry + 1 >= max_pass_retries) {
            PrintLogBegin(llvm::errs(), ctx);
            llvm::errs() << llvm::formatv(
                "WARNING: Maximum number of passes ({0}) reached, consider increasing the limit\n", max_pass_retries);
          }
          continue;
        }
        case WhatsNext::Abort: {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Abort requested\n");
          return 1;
        }
        default: {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Unknown WhatsNext value: {0}\n",
                                        std::to_underlying(ctx.whats_next.value()));
          return 1;
        }
        }

        // NOLINTNEXTLINE(readability-simplify-boolean-expr)
        if (false) {
        move_to_next_pass:
          if (!end_at_pass.empty() && *ctx.action_name == end_at_pass) {
            PrintLogBeginShort(llvm::outs(), initial_file);
            llvm::outs() << llvm::formatv("End at pass requested due to \"end\" option\n");
            goto move_to_next_file;
          }
          break;
        }
      }

      // NOLINTNEXTLINE(readability-simplify-boolean-expr)
      if (false) {
      move_to_next_file:
        break;
      }
    }

    const std::string final_file = llvm::formatv("{0}.c2pnk.c", initial_file);
    if (std::error_code const ec =
            llvm::sys::fs::copy_file(llvm::Twine(initial_file + current_suffix), llvm::Twine(final_file))) {
      PrintLogBeginShort(llvm::errs(), initial_file);
      llvm::errs() << llvm::formatv("FATAL: Failed to copy {0} to {1}: {2}\n", initial_file + current_suffix,
                                    final_file, ec.message());
      return 1;
    }

    PrintLogBeginShort(llvm::outs(), initial_file);
    llvm::outs() << llvm::formatv("All passes complete, final output at {0}\n", final_file);
  }

  llvm::outs() << "[c2pancake] All files completed\n";
  return 0;
}
