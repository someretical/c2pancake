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

#include <memory>
#include <string>
#include <utility>

namespace pancake {
// This is what all passes should inherit from
class C2PancakePass : public clang::ASTConsumer {
protected:
  const clang::CompilerInstance &ci;
  std::string in_file;
  PipelineStageCtx &ps_ctx;
  size_t tmp_var_counter = 0;

public:
  explicit C2PancakePass(const clang::CompilerInstance &CI, llvm::StringRef in_file, PipelineStageCtx &ps_ctx)
      : ci(CI), in_file(in_file), ps_ctx(ps_ctx) {}

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, ps_ctx.major_pass_number, ps_ctx.minor_pass_number,
                         tmp_var_counter++);
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

// T is used for CRTP for GetActionName
template <typename T>
  requires std::derived_from<T, C2PancakePass>
class PipelineStage : public clang::ASTFrontendAction {
  PipelineStageCtx &ps_ctx;

public:
  explicit PipelineStage(PipelineStageCtx &ps_ctx) : ps_ctx(ps_ctx) {}

  auto CreateASTConsumer(clang::CompilerInstance &compiler, llvm::StringRef in_file)
      -> std::unique_ptr<clang::ASTConsumer> override {
    return std::make_unique<T>(compiler, in_file, ps_ctx);
  }

  auto EndSourceFileAction() -> void override {
    if (ps_ctx.error) {
      PrintLogBegin(llvm::errs(), ps_ctx);
      llvm::errs() << "Not writing output file due to error";
      return;
    }

    if (ps_ctx.replacements.empty()) {
      PrintLogBegin(llvm::outs(), ps_ctx);
      llvm::outs() << "No replacements to apply, skipping output file write\n";
      return;
    }

    auto &sm = getCompilerInstance().getSourceManager();
    auto current_filename = getCurrentInput().getFile();

    auto buf = sm.getBufferOrFake(sm.getMainFileID());
    auto source_text = buf.getBuffer();
    auto original_filename = current_filename.drop_back(ps_ctx.current_suffix.size());
    auto output_path = llvm::formatv("{0}{1}", original_filename, ps_ctx.next_suffix).str();

    auto unformatted = clang::tooling::applyAllReplacements(source_text, ps_ctx.replacements);
    if (auto error = unformatted.takeError()) {
      ps_ctx.end_src_file_action_error = CreateRuntimeError(
          llvm::formatv("Failed to apply replacements INSIDE the pipeline: {0}\n    THIS SHOULD NEVER HAPPEN!",
                        llvm::fmt_consume(std::move(error))));
      return;
    }

    // apply clang-format pass
    auto style =
        clang::format::getStyle("file", output_path, "LLVM", *unformatted, &sm.getFileManager().getVirtualFileSystem());
    if (auto error = style.takeError()) {
      ps_ctx.end_src_file_action_error = CreateRuntimeError(
          llvm::formatv("clang-format style lookup failed: {0}\n", llvm::fmt_consume(std::move(error))));
      return;
    }

    // no way to avoid the cast from unsigned long to unsigned int
    llvm::SmallVector<clang::tooling::Range, 1> ranges{clang::tooling::Range(0, (unsigned)unformatted->size())};
    auto format_replacements = clang::format::reformat(*style, *unformatted, ranges);
    auto formatted = clang::tooling::applyAllReplacements(*unformatted, format_replacements);
    if (auto error = formatted.takeError()) {
      ps_ctx.end_src_file_action_error =
          CreateRuntimeError(llvm::formatv("clang-format apply failed: {0}\n", llvm::fmt_consume(std::move(error))));
      return;
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
    if (!ec) {
      out << *formatted;
      ps_ctx.file_modified = true;
      PrintLogBegin(llvm::outs(), ps_ctx);
      llvm::outs() << llvm::formatv("Applied {0} replacement{1}, output written to {2}\n", ps_ctx.replacements.size(),
                                    ps_ctx.replacements.size() != 1 ? "s" : "", output_path);
    } else {
      ps_ctx.end_src_file_action_error =
          CreateRuntimeError(llvm::formatv("Failed to open output file {0}: {1}", output_path, ec.message()));
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
    virtual auto BetterCreate(PipelineStageCtx &ctx) -> std::unique_ptr<clang::FrontendAction> = 0;
  };

  template <typename T> struct FactoryFactory : public AbstractFactory {
    auto create() -> std::unique_ptr<clang::FrontendAction> override {
      llvm_unreachable("PipelineAction factories require a PipelineActionCtx, use BetterCreate instead");
    }
    auto BetterCreate(PipelineStageCtx &ctx) -> std::unique_ptr<clang::FrontendAction> override {
      return std::make_unique<T>(ctx);
    }
  };

  clang::tooling::CommonOptionsParser &options_parser;
  std::vector<std::unique_ptr<AbstractFactory>> factories;

public:
  explicit Pipeline(clang::tooling::CommonOptionsParser &options_parser) : options_parser(options_parser) {}

  template <typename T> void AddStage() { factories.emplace_back(std::make_unique<FactoryFactory<T>>()); }

  auto Run() -> int;
};
} // namespace pancake

#endif // C2PANCAKE_MULTIPASS_H
