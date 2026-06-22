#include "MultiPass.h"

using namespace pancake;

auto Pipeline::Run() -> int {
  StagedCompilationDatabase db(options_parser.getCompilations(), "");
  const std::vector<std::string> initial_files = options_parser.getSourcePathList();
  auto current_files = initial_files;

  for (auto &&[i, factory] : std::views::enumerate(factories)) {
    factory->cur_suffix = db.current_suffix;
    factory->next_suffix = std::format(".c2pnk.{}.c", i);

    clang::tooling::ClangTool tool(db, current_files);
    if (tool.run(factory.get()) != 0) {
      llvm::errs() << "Error: " << factory->GetActionName() << " failed.\n";
      return 1;
    }

    llvm::outs() << llvm::formatv("c2pancake: Pass {0} completed\n", factory->GetActionName());

    db.current_suffix = factory->next_suffix;
    current_files =
        std::views::transform(initial_files, [&](const auto &file) -> auto { return file + db.current_suffix; }) |
        std::ranges::to<std::vector>();
  }

  llvm::outs() << "c2pancake: All passes completed successfully.\n";
  return 0;
}