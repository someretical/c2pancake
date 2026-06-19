#include "MultiPass.h"
#include "CodeGen.h"
#include "IRBuilder.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <format>
#include <llvm/ADT/RewriteBuffer.h>
#include <llvm/ADT/StringRef.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <print>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace pancake;

auto MultiPassConsumer::HandleTranslationUnit(clang::ASTContext &Ctx) -> void {
  // TODO: transform function calls which take stack pointers
  // TODO transform for loops into while loops

  IRBuilder ir_builder(R, Ctx);
  ir_builder.TraverseDecl(Ctx.getTranslationUnitDecl());
  CodeGen code_gen;
  auto output = code_gen.generate(ir_builder);

  auto output_file = std::format("{}.pnk", file);
  std::error_code ec;
  llvm::raw_fd_ostream out(output_file, ec, llvm::sys::fs::OF_None);
  if (ec) {
    llvm::errs() << "Could not open file: " << ec.message() << "\n";
    return;
  }

  out << output;
  std::println("Wrote to {}", output_file);
}

auto MultiPassAction::CreateASTConsumer(clang::CompilerInstance &CI,
                                        llvm::StringRef file)
    -> std::unique_ptr<clang::ASTConsumer> {
  R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  std::string const copy = file.str();
  return std::make_unique<MultiPassConsumer>(R, copy);
}
auto MultiPassAction::EndSourceFileAction() -> void {}

namespace {
class MultiPassCompilationDatabase
    : public clang::tooling::CompilationDatabase {
private:
  const clang::tooling::CompilationDatabase &InnerDB;
  std::string CurrentSuffix;

  auto getOriginalFilename(llvm::StringRef Filename) const -> std::string {
    llvm::StringRef const ref(Filename);
    if (!CurrentSuffix.empty() && ref.ends_with(CurrentSuffix)) {
      return ref.drop_back(CurrentSuffix.size()).str();
    }
    return ref.str();
  }

public:
  MultiPassCompilationDatabase(const CompilationDatabase &DB,
                               std::string Suffix)
      : InnerDB(DB), CurrentSuffix(std::move(Suffix)) {}

  auto getCompileCommands(llvm::StringRef Filename) const
      -> std::vector<clang::tooling::CompileCommand> override {
    auto original_file = getOriginalFilename(Filename);

    auto commands = InnerDB.getCompileCommands(original_file);

    for (auto &command : commands) {
      if (command.Filename == original_file) {
        command.Filename = Filename.str();
      }

      for (auto &arg : command.CommandLine) {
        if (arg == original_file) {
          arg = Filename.str();
        }
      }
    }
    return commands;
  }

  auto getAllFiles() const -> std::vector<std::string> override {
    return InnerDB.getAllFiles();
  }

  auto getAllCompileCommands() const
      -> std::vector<clang::tooling::CompileCommand> override {
    return InnerDB.getAllCompileCommands();
  }
};
} // namespace

auto pancake::runMultiPass(
    const clang::tooling::CompilationDatabase &BaseCompilations,
    const std::vector<std::string> &InputFiles,
    const std::string &CurrentSuffix, // Suffix on the files we are READING
    const std::string &NextSuffix,    // Suffix we append for the next pass
    std::unique_ptr<clang::tooling::FrontendActionFactory> ActionFactory,
    const std::string &PassName) -> std::vector<std::string> {
  llvm::outs() << std::format(">> Running {} on {} files...\n", PassName,
                              InputFiles.size());

  // Wrap the compilation database for this specific pipeline step
  MultiPassCompilationDatabase const multi_pass_db(BaseCompilations,
                                                   CurrentSuffix);

  // Initialize the tool with our robust wrapper database
  clang::tooling::ClangTool tool(multi_pass_db, InputFiles);

  if (tool.run(ActionFactory.get()) != 0) {
    llvm::errs() << "Error: " << PassName << " failed.\n";
    return {};
  }

  // Prepare next step tracking
  std::vector<std::string> output_files;
  output_files.reserve(InputFiles.size());
  for (const auto &file : InputFiles) {
    output_files.push_back(file + NextSuffix);
  }
  return output_files;
}
