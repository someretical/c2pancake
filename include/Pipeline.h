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

#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace pancake {
class StagedCompilationDatabase : public clang::tooling::CompilationDatabase {
private:
  clang::tooling::CompilationDatabase &base_db;

  auto GetOriginalFilename(llvm::StringRef Filename) const -> llvm::StringRef;

public:
  std::string current_suffix;

  StagedCompilationDatabase(clang::tooling::CompilationDatabase &db, std::string suffix)
      : base_db(db), current_suffix(std::move(suffix)) {}

  auto getCompileCommands(llvm::StringRef Filename) const -> std::vector<clang::tooling::CompileCommand> override;

  auto getAllFiles() const -> std::vector<std::string> override { return base_db.getAllFiles(); }

  auto getAllCompileCommands() const -> std::vector<clang::tooling::CompileCommand> override {
    return base_db.getAllCompileCommands();
  }
};

/*
A new ASTFrontendAction is created for each TU for each pass
So there's a 1-1 relationship between PipelineAction and C2PancakePass
The Ctx is created inside of Pipeline::Run and passed by reference to each PipelineAction and then C2PancakePass
*/
enum class FailureBehaviour : uint8_t { NONE, Continue, Repeat };
enum class FailureMode : uint8_t { None, Repeat, Fatal };
struct PipelineActionCtx {
  size_t pass_number;
  clang::tooling::Replacements replacements;
  std::string action_name; // set by the PipelineAction constructor
  std::string current_suffix;
  std::string next_suffix;
  FailureBehaviour failure_behaviour = FailureBehaviour::NONE; // set by the PipelineAction constructor
  FailureMode failure_mode = FailureMode::None;

  explicit PipelineActionCtx(size_t pass_number, std::string cur_suffix, std::string next_suffix)
      : pass_number(pass_number), current_suffix(std::move(cur_suffix)), next_suffix(std::move(next_suffix)) {}
};

inline auto LogBegin(const PipelineActionCtx &ctx, const std::string &in_file) {
  return llvm::formatv("[c2pancake] {0}: Pass {1}, iter {2}:", in_file, ctx.action_name, ctx.pass_number);
}

inline auto LogBeginShort(const std::string &in_file) { return llvm::formatv("[c2pancake] {0}:", in_file); }

// This is what all passes should inherit from
class C2PancakePass : public clang::ASTConsumer {
protected:
  const clang::CompilerInstance &ci;
  std::string in_file;
  PipelineActionCtx &pa_ctx;

public:
  explicit C2PancakePass(const clang::CompilerInstance &CI, llvm::StringRef in_file, PipelineActionCtx &pa_ctx)
      : ci(CI), in_file(in_file), pa_ctx(pa_ctx) {}

  // children must implement HandleTranslationUnit
};

// T is used for CRTP for GetActionName
template <typename T>
  requires std::derived_from<T, C2PancakePass>
class PipelineAction : public clang::ASTFrontendAction {
  PipelineActionCtx &pa_ctx;

public:
  explicit PipelineAction(PipelineActionCtx &pa_ctx) : pa_ctx(pa_ctx) {}

  auto CreateASTConsumer(clang::CompilerInstance &compiler, llvm::StringRef in_file)
      -> std::unique_ptr<clang::ASTConsumer> override {
    return std::make_unique<T>(compiler, in_file, pa_ctx);
  }

  auto EndSourceFileAction() -> void override {
    auto &sm = getCompilerInstance().getSourceManager();
    auto current_filename = getCurrentInput().getFile();

    auto buf = sm.getBufferOrFake(sm.getMainFileID());
    std::string source_text(buf.getBuffer());

    auto result = applyAllReplacements(source_text, pa_ctx.replacements);
    if (!result) {
      // abnormal error!
      llvm::errs() << llvm::formatv("[c2pancake] {0}: {1} Failed to apply replacements: {2}\n", current_filename,
                                    pa_ctx.action_name, current_filename, llvm::toString(result.takeError()));
      return;
    }

    auto original_filename = current_filename.drop_back(pa_ctx.current_suffix.size());
    std::string output_path = llvm::formatv("{0}{1}", original_filename, pa_ctx.next_suffix);
    std::error_code ec;
    llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
    if (!ec) {
      out << *result;
    } else {
      llvm::errs() << llvm::formatv("{0} Failed to open output file '{3}': {4}\n", LogBegin(pa_ctx, current_filename),
                                    output_path, ec.message());
    }
  }
};

class Pipeline {
private:
  struct AbstractFactory : public clang::tooling::FrontendActionFactory {
    AbstractFactory() = default;
    ~AbstractFactory() override = default;
    AbstractFactory(const AbstractFactory &) = delete;
    auto operator=(const AbstractFactory &) -> AbstractFactory & = delete;
    AbstractFactory(AbstractFactory &&) = delete;
    auto operator=(AbstractFactory &&) -> AbstractFactory & = delete;
    virtual auto BetterCreate(PipelineActionCtx &ctx) -> std::unique_ptr<clang::FrontendAction> = 0;
  };

  template <typename T> struct FactoryFactory : public AbstractFactory {
    auto create() -> std::unique_ptr<clang::FrontendAction> override {
      llvm_unreachable("PipelineAction factories require a PipelineActionCtx, use BetterCreate instead");
    }
    auto BetterCreate(PipelineActionCtx &ctx) -> std::unique_ptr<clang::FrontendAction> override {
      return std::make_unique<T>(ctx);
    }
  };

  clang::tooling::CommonOptionsParser &options_parser;
  std::vector<std::unique_ptr<AbstractFactory>> factories;

public:
  explicit Pipeline(clang::tooling::CommonOptionsParser &options_parser) : options_parser(options_parser) {}

  template <typename T> void AddPass() { factories.emplace_back(std::make_unique<FactoryFactory<T>>()); }

  auto Run() -> int;
};
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
