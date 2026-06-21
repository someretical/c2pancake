#include "MultiPass.h"
#include "Pass_HoistArraysAndAddresses.h"
#include "Pass_PromoteRecords.h"

#include <clang/Basic/LLVM.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pancake;

namespace {
llvm::cl::OptionCategory c2_pancake_options("c2pancake options");
const llvm::cl::extrahelp
    common_help(clang::tooling::CommonOptionsParser::HelpMessage);
const llvm::cl::extrahelp more_help("\nMore help text...\n");
} // namespace

auto main(int argc, const char **argv) -> int {

  auto expected_parser = clang::tooling::CommonOptionsParser::create(
      argc, argv, c2_pancake_options);
  if (!expected_parser) {
    llvm::errs() << expected_parser.takeError();
    return 1;
  }
  auto &base_db = expected_parser->getCompilations();
  std::vector<std::string> current_files = expected_parser->getSourcePathList();

  int cur_pass = 0;

  {
    using namespace pancake::pass_promote_records;
    auto factory = std::make_unique<ActionFactory>();
    const std::string cur_suffix;
    auto next_suffix = std::format(".c2pnk.{}.c", cur_pass + 1);
    factory->cur_suffix = cur_suffix;
    factory->next_suffix = next_suffix;
    current_files =
        runMultiPass(base_db, current_files, cur_suffix, next_suffix,
                     std::move(factory), "Promote records");
    if (current_files.empty())
      return 1;
    cur_pass++;
  }

  {
    using namespace pancake::pass_hoist_arrays_and_addresses;
    auto factory = std::make_unique<ActionFactory>();
    const std::string cur_suffix = std::format(".c2pnk.{}.c", cur_pass);
    auto next_suffix = std::format(".c2pnk.{}.c", cur_pass + 1);
    factory->cur_suffix = cur_suffix;
    factory->next_suffix = next_suffix;
    current_files =
        runMultiPass(base_db, current_files, cur_suffix, next_suffix,
                     std::move(factory), "Hoist arrays and addresses");
    if (current_files.empty())
      return 1;
    cur_pass++;
  }

  return 0;
}