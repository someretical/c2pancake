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
    PipelineRunCtx run_ctx(initial_file);

    PrintLogBeginShort(llvm::outs(), initial_file);
    llvm::outs() << "Starting processing\n";
    bool start_at_pass_found = start_at_pass.empty();
    bool move_to_next_file = false;

    for (size_t pass_num = 0; pass_num < factories.size() && !move_to_next_file; ++pass_num) {
      auto &factory = factories.at(pass_num);
      for (size_t pass_retry = 0; pass_retry < max_pass_retries; ++pass_retry) {
        bool advance_pass = false;
        std::string next_suffix = llvm::formatv(".{0}-{1}-c2pnk.c", pass_num, pass_retry);
        const StagedCompilationDatabase db(options_parser.getCompilations(), current_suffix);
        const std::string current_file = llvm::formatv("{0}{1}", initial_file, current_suffix);
        const std::string next_file = llvm::formatv("{0}{1}", initial_file, next_suffix);
        std::vector<clang::tooling::CompileCommand> commands;
        std::unique_ptr<clang::tooling::ClangTool> tool;
        if (factory->Kind() == StageKind::Clang) {
          tool = std::make_unique<clang::tooling::ClangTool>(
              db, llvm::SmallVector<std::string, 1>{current_file});
          commands = db.getCompileCommands(current_file);
          if (commands.empty()) {
            PrintLogBeginShort(llvm::errs(), current_file);
            llvm::errs() << "FATAL: No compile command found for input file\n";
            return 1;
          }
          if (commands.size() > 1) {
            PrintLogBeginShort(llvm::errs(), current_file);
            llvm::errs() << llvm::formatv("WARNING: {0} (>1) compile commands found, only executing the first one\n",
                                          commands.size());
            for (const auto [i, cmd] : commands | std::views::enumerate) {
              llvm::errs() << llvm::formatv("    {0}: {1}\n", i, llvm::join(cmd.CommandLine, " "));
            }
          }
        }

        StageContext ctx(run_ctx, pass_num, pass_retry, current_file, current_suffix, next_file, next_suffix);
        auto action = factory->CreateAction(ctx);
        if (!ctx.stage_name.has_value() && factory->Kind() != StageKind::Clang) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << "FATAL: File stage did not set a stage name\n";
          return 1;
        }
        if (factory->Kind() == StageKind::Clang && !ctx.stage_name.has_value()) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Stage did not set a stage name\n");
          return 1;
        }

        if (!start_at_pass_found) {
          if (*ctx.stage_name == start_at_pass) {
            start_at_pass_found = true;
          } else {
            PrintLogBegin(llvm::outs(), ctx);
            llvm::outs() << "Skipping pass because name didn't match \"start\" option\n";
            advance_pass = true;
            break;
          }
        }

        if (factory->Kind() == StageKind::Clang) {
          clang::tooling::ToolInvocation invocation(commands.front().CommandLine, std::move(action),
                                                    &tool->getFiles());
          if (!invocation.run()) {
            PrintLogBegin(llvm::errs(), ctx);
            llvm::errs() << llvm::formatv("FATAL: ToolInvocation.run() failed\n");
            return 1;
          }
        } else if (auto error = factory->Run(ctx)) {
          ctx.error = std::move(error);
        }

        if (!ctx.HasControl()) {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Stage did not set a control outcome\n");
          return 1;
        }

        if (ctx.error) {
          // runtime error
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << ctx.error;
          return 1;
        }

        run_ctx.generated_files.insert(run_ctx.generated_files.end(), ctx.result.generated_files.begin(),
                                       ctx.result.generated_files.end());

        switch (ctx.result.control) {
        case StageControl::NextFile: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Move to next file requested\n");
          move_to_next_file = true;
          break;
        }
        case StageControl::Continue: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Move to next pass requested\n");
          if (ctx.result.file_modified) {
            current_suffix = next_suffix;
          }
          advance_pass = true;
          break;
        }
        case StageControl::Repeat: {
          PrintLogBegin(llvm::outs(), ctx);
          llvm::outs() << llvm::formatv("Repeat pass requested\n");

          if (!ctx.result.file_modified) {
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
        case StageControl::Abort: {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Abort requested\n");
          return 1;
        }
        default: {
          PrintLogBegin(llvm::errs(), ctx);
          llvm::errs() << llvm::formatv("FATAL: Unknown stage control value: {0}\n",
                                        std::to_underlying(ctx.result.control));
          return 1;
        }
        }

        if (advance_pass || move_to_next_file) {
          if (!end_at_pass.empty() && *ctx.stage_name == end_at_pass) {
            PrintLogBeginShort(llvm::outs(), initial_file);
            llvm::outs() << llvm::formatv("End at pass requested due to \"end\" option\n");
            move_to_next_file = true;
          }
          break;
        }
      }
    }

    const std::string final_file = run_ctx.generated_files.empty()
                                        ? llvm::formatv("{0}.c2pnk.c", initial_file).str()
                                        : run_ctx.generated_files.back();
    if (!run_ctx.generated_files.empty()) {
      PrintLogBeginShort(llvm::outs(), initial_file);
      llvm::outs() << llvm::formatv("All passes complete, final output at {0}\n", final_file);
      continue;
    }
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
