#ifndef C2PANCAKE_MULTIPASS_H
#define C2PANCAKE_MULTIPASS_H

#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Format/Format.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm-22/llvm/Support/ErrorHandling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>

#include <concepts>
#include <memory>
#include <string>
#include <utility>

namespace pancake {
template <typename T>
concept ClangStageType = std::derived_from<T, clang::FrontendAction> && std::constructible_from<T, StageContext &>;

template <typename T>
concept FileStageType = std::constructible_from<T, StageContext &> && requires(T &stage) {
  { stage.Run() } -> std::same_as<llvm::Error>;
};

// This is what all passes should inherit from
class C2PancakePass : public clang::ASTConsumer {
protected:
  const clang::CompilerInstance &ci;
  std::string in_file;
  StageContext &ps_ctx;
  size_t tmp_var_counter = 0;

public:
  explicit C2PancakePass(const clang::CompilerInstance &CI, llvm::StringRef in_file, StageContext &ps_ctx)
      : ci(CI), in_file(in_file), ps_ctx(ps_ctx) {}

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, ps_ctx.stage_index, ps_ctx.attempt_index, tmp_var_counter++);
  }

  static auto PrintType(clang::ASTContext &context, const clang::QualType ty, const llvm::StringRef var_name)
      -> std::string {
    std::string s;
    llvm::raw_string_ostream os(s);
    ty.print(os, context.getPrintingPolicy(), var_name);
    return os.str();
  }

  // children must implement HandleTranslationUnit
  void HandleTranslationUnit(clang::ASTContext &Ctx) override = 0;
};

struct ReplacementOutputFinalizer {
  static auto Write(StageContext &ctx, clang::CompilerInstance &compiler) -> llvm::Error {
    if (ctx.replacements.empty()) {
      PrintLogBegin(llvm::outs(), ctx);
      llvm::outs() << "No replacements to apply, skipping output file write\n";
      return llvm::Error::success();
    }

    auto &sm = compiler.getSourceManager();
    auto current_filename = compiler.getFrontendOpts().Inputs.front().getFile();
    auto buf = sm.getBufferOrFake(sm.getMainFileID());
    auto source_text = buf.getBuffer();
    auto original_filename = current_filename.drop_back(ctx.current_suffix.size());
    auto output_path = llvm::formatv("{0}{1}", original_filename, ctx.next_suffix).str();

    auto unformatted = clang::tooling::applyAllReplacements(source_text, ctx.replacements);
    if (auto error = unformatted.takeError()) {
      return CreateRuntimeError(
          llvm::formatv("Failed to apply replacements INSIDE the pipeline: {0}\n    THIS SHOULD NEVER HAPPEN!",
                        llvm::fmt_consume(std::move(error))));
    }

    auto style =
        clang::format::getStyle("file", output_path, "LLVM", *unformatted, &sm.getFileManager().getVirtualFileSystem());
    if (auto error = style.takeError()) {
      return CreateRuntimeError(
          llvm::formatv("clang-format style lookup failed: {0}\n", llvm::fmt_consume(std::move(error))));
    }

    llvm::SmallVector<clang::tooling::Range, 1> ranges{clang::tooling::Range(0, (unsigned)unformatted->size())};
    auto format_replacements = clang::format::reformat(*style, *unformatted, ranges);
    auto formatted = clang::tooling::applyAllReplacements(*unformatted, format_replacements);
    if (auto error = formatted.takeError()) {
      return CreateRuntimeError(llvm::formatv("clang-format apply failed: {0}\n", llvm::fmt_consume(std::move(error))));
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
    if (ec) {
      return CreateRuntimeError(llvm::formatv("Failed to open output file {0}: {1}", output_path, ec.message()));
    }

    out << *formatted;
    ctx.MarkModified();
    ctx.SetOutputFile(output_path);
    PrintLogBegin(llvm::outs(), ctx);
    llvm::outs() << llvm::formatv("Applied {0} replacement{1}, output written to {2}\n", ctx.replacements.size(),
                                  ctx.replacements.size() != 1 ? "s" : "", output_path);
    return llvm::Error::success();
  }
};

// T is used for CRTP for GetActionName
template <typename T, typename Finalizer = ReplacementOutputFinalizer>
  requires std::derived_from<T, C2PancakePass>
class ClangStage : public clang::ASTFrontendAction {
  StageContext &ps_ctx;

public:
  explicit ClangStage(StageContext &ps_ctx) : ps_ctx(ps_ctx) {}

  auto CreateASTConsumer(clang::CompilerInstance &compiler, llvm::StringRef in_file)
      -> std::unique_ptr<clang::ASTConsumer> override {
    return std::make_unique<T>(compiler, in_file, ps_ctx);
  }

  auto EndSourceFileAction() -> void override {
    if (ps_ctx.error) {
      PrintLogBegin(llvm::errs(), ps_ctx);
      llvm::errs() << "Not writing output file due to error\n";
      return;
    }
    if (auto error = Finalizer::Write(ps_ctx, getCompilerInstance())) {
      ps_ctx.error = std::move(error);
    }
  }
};

class Pipeline {
private:
  struct StageFactory {
    StageFactory() = default;
    virtual ~StageFactory() = default;
    StageFactory(const StageFactory &) = delete;
    auto operator=(const StageFactory &) -> StageFactory & = delete;
    StageFactory(StageFactory &&) = delete;
    auto operator=(StageFactory &&) -> StageFactory & = delete;
    virtual auto Kind() const -> StageKind = 0;
    virtual auto CreateAction(StageContext &ctx) -> std::unique_ptr<clang::FrontendAction> = 0;
    virtual auto Run(StageContext &ctx) -> llvm::Error = 0;
  };

  template <ClangStageType T> struct ClangStageFactory : public StageFactory {
    auto Kind() const -> StageKind override { return StageKind::Clang; }
    auto CreateAction(StageContext &ctx) -> std::unique_ptr<clang::FrontendAction> override {
      return std::make_unique<T>(ctx);
    }
    auto Run(StageContext &) -> llvm::Error override { return llvm::Error::success(); }
  };

  template <FileStageType T> struct FileStageFactory : public StageFactory {
    auto Kind() const -> StageKind override { return StageKind::File; }
    auto CreateAction(StageContext &) -> std::unique_ptr<clang::FrontendAction> override { return nullptr; }
    auto Run(StageContext &ctx) -> llvm::Error override {
      T stage(ctx);
      return stage.Run();
    }
  };

  clang::tooling::CommonOptionsParser &options_parser;
  std::vector<std::unique_ptr<StageFactory>> factories;

public:
  explicit Pipeline(clang::tooling::CommonOptionsParser &options_parser) : options_parser(options_parser) {}

  template <ClangStageType T> void AddStage() { factories.emplace_back(std::make_unique<ClangStageFactory<T>>()); }

  // File stages do not invoke Clang. T must construct from StageContext&
  // and provide auto Run() -> llvm::Error.
  template <FileStageType T> void AddFileStage() { factories.emplace_back(std::make_unique<FileStageFactory<T>>()); }

  auto Run() -> int;
};
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
