#ifndef C2PANCAKE_MULTIPASS_H
#define C2PANCAKE_MULTIPASS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm-22/llvm/Support/FormatVariadic.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <string>
#include <utility>

namespace pancake {
class StagedCompilationDatabase : public clang::tooling::CompilationDatabase {
private:
  clang::tooling::CompilationDatabase &base_db;

  auto GetOriginalFilename(llvm::StringRef Filename) const -> llvm::StringRef {
    const auto ref = Filename;
    if (!current_suffix.empty() && ref.ends_with(current_suffix)) {
      return ref.drop_back(current_suffix.size());
    }
    return ref;
  }

public:
  std::string current_suffix;

  StagedCompilationDatabase(clang::tooling::CompilationDatabase &db, std::string suffix)
      : base_db(db), current_suffix(std::move(suffix)) {}

  auto getCompileCommands(llvm::StringRef Filename) const -> std::vector<clang::tooling::CompileCommand> override {
    auto original_file = GetOriginalFilename(Filename);

    auto commands = base_db.getCompileCommands(original_file);

    for (auto &command : commands) {
      if (command.Filename == original_file) {
        command.Filename = Filename;
      }

      for (auto &arg : command.CommandLine) {
        if (arg == original_file) {
          arg = Filename;
        }
      }
    }
    return commands;
  }

  auto getAllFiles() const -> std::vector<std::string> override { return base_db.getAllFiles(); }

  auto getAllCompileCommands() const -> std::vector<clang::tooling::CompileCommand> override {
    return base_db.getAllCompileCommands();
  }
};

// This is what all passes should inherit from
class C2PancakePass : public clang::ASTConsumer {
private:
  const clang::CompilerInstance &CI;
  llvm::StringRef in_file;

protected:
  clang::tooling::Replacements &repls;

public:
  explicit C2PancakePass(const clang::CompilerInstance &CI, llvm::StringRef in_file,
                         clang::tooling::Replacements &repls)
      : CI(CI), in_file(in_file), repls(repls) {}

  // children must implement HandleTranslationUnit
};

// T is used for CRTP for GetActionName
template <typename T, typename U>
  requires std::derived_from<U, C2PancakePass>
class PipelineAction : public clang::ASTFrontendAction {
  clang::tooling::Replacements repls;
  std::string current_suffix;
  std::string next_suffix;

public:
  explicit PipelineAction(std::string cur_suffix, std::string next_suffix)
      : current_suffix(std::move(cur_suffix)), next_suffix(std::move(next_suffix)) {}

  auto GetActionName() const -> std::string { return T::GetActionName(); }

  auto CreateASTConsumer(clang::CompilerInstance &compiler, llvm::StringRef in_file)
      -> std::unique_ptr<clang::ASTConsumer> override {
    return std::make_unique<U>(compiler, in_file, repls);
  }

  auto EndSourceFileAction() -> void override {
    auto &sm = getCompilerInstance().getSourceManager();
    auto opt = sm.getNonBuiltinFilenameForID(sm.getMainFileID());
    assert(opt.has_value() && "Expected main file to have a filename");
    auto current_filename = *opt;

    auto buf = sm.getBufferOrFake(sm.getMainFileID());
    std::string source_text(buf.getBuffer());

    auto result = applyAllReplacements(source_text, repls);
    if (!result) {
      llvm::errs() << llvm::formatv("{0}: {1} Failed to apply replacements: {2}\n", GetActionName(), current_filename,
                                    llvm::toString(result.takeError()));
      return;
    }

    auto original_filename = current_filename.drop_back(current_suffix.size());
    std::string output_path = llvm::formatv("{0}{1}", original_filename, next_suffix);
    std::error_code ec;
    llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
    if (!ec) {
      out << *result;
    } else {
      llvm::errs() << llvm::formatv("{0}: {1} Failed to open output file '{2}': {3}\n", GetActionName(),
                                    current_filename, output_path, ec.message());
    }
  }
};

class Pipeline {
private:
  struct FactoryFactory : public clang::tooling::FrontendActionFactory {
    std::string cur_suffix;
    std::string next_suffix;
    ~FactoryFactory() override = default;
    auto create() -> std::unique_ptr<clang::FrontendAction> override = 0;
    virtual auto GetActionName() const -> std::string = 0;
  };

  // actual extreme fuckery because of llvm
  template <typename T> struct FinalFactory : public FactoryFactory {
    auto create() -> std::unique_ptr<clang::FrontendAction> override {
      return std::make_unique<T>(cur_suffix, next_suffix);
    }

    auto GetActionName() const -> std::string override { return T::GetActionName(); }
  };

  clang::tooling::CommonOptionsParser &options_parser;
  std::vector<std::unique_ptr<FactoryFactory>> factories;

public:
  explicit Pipeline(clang::tooling::CommonOptionsParser &options_parser) : options_parser(options_parser) {}

  template <typename T> void AddPass() { factories.emplace_back(std::make_unique<FinalFactory<T>>()); }

  auto Run() -> int;
};
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
