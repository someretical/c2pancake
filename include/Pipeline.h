#ifndef C2PANCAKE_MULTIPASS_H
#define C2PANCAKE_MULTIPASS_H

#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>

#include <cassert>
#include <memory>
#include <string>

namespace pancake {
// This is what all passes should inherit from
class C2PancakePass : public clang::ASTConsumer {
protected:
  const clang::CompilerInstance &ci;
  std::string in_file;
  PipelineActionCtx &pa_ctx;
  size_t tmp_var_counter = 0;

public:
  explicit C2PancakePass(const clang::CompilerInstance &CI, llvm::StringRef in_file, PipelineActionCtx &pa_ctx)
      : ci(CI), in_file(in_file), pa_ctx(pa_ctx) {}

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, pa_ctx.major_pass_number, pa_ctx.minor_pass_number,
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
    if (!pa_ctx.action_type.has_value()) {
      llvm_unreachable("Action did not set a failure behaviour");
    }

    switch (pa_ctx.action_type.value()) {
    case PipelineActionType::Analyser:
      // nothing to do for analysers
      break;
    case PipelineActionType::Rewriter: {
      auto &sm = getCompilerInstance().getSourceManager();
      auto current_filename = getCurrentInput().getFile();

      auto buf = sm.getBufferOrFake(sm.getMainFileID());
      std::string source_text(buf.getBuffer());

      auto result = applyAllReplacements(source_text, pa_ctx.replacements);
      if (!result) {
        // abnormal error!
        llvm::errs() << llvm::formatv("{0} {1}\n", LogBegin(pa_ctx), llvm::fmt_consume(result.takeError()));
        break;
      }

      auto original_filename = current_filename.drop_back(pa_ctx.current_suffix.size());
      std::string output_path = llvm::formatv("{0}{1}", original_filename, pa_ctx.next_suffix);
      std::error_code ec;
      llvm::raw_fd_ostream out(output_path, ec, llvm::sys::fs::OF_None);
      if (!ec) {
        out << *result;
      } else {
        llvm::errs() << llvm::formatv("{0} {1}\n", LogBegin(pa_ctx), ec.message());
      }

      break;
    }
    default: {
      llvm_unreachable("Unknown PipelineActionType");
    }
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
