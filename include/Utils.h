#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/Support/FormatVariadic.h>

#include <string>

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
enum class FailureBehaviour : uint8_t { MoveToNextPass, RepeatPass, MoveToNextFile };
enum class FailureMode : uint8_t { Success, RepeatPass, Fail };
enum class PipelineActionType : uint8_t { None, Rewriter, Analyser };
struct PipelineActionCtx {
  const size_t &pass_number;         // provided by Pipeline::Run
  const std::string &current_file;   // provided by Pipeline::Run
  const std::string &current_suffix; // provided by Pipeline::Run
  const std::string &next_file;      // provided by Pipeline::Run
  const std::string &next_suffix;    // provided by Pipeline::Run

  std::optional<std::string> action_name;            // set by the PipelineAction constructor
  std::optional<PipelineActionType> action_type;     // set by the PipelineAction constructor
  std::optional<FailureBehaviour> failure_behaviour; // set by the PipelineAction constructor
  clang::tooling::Replacements &replacements;        // provided by Pipeline::Run, the pass adds to it
  FailureMode failure_mode = FailureMode::Success;   // set by the pass itself at the end of HandleTranslationUnit

  explicit PipelineActionCtx(const size_t &pass_number, clang::tooling::Replacements &replacements,
                             const std::string &current_file, const std::string &current_suffix,
                             const std::string &next_file, const std::string &next_suffix)
      : pass_number(pass_number), current_file(current_file), current_suffix(current_suffix), next_file(next_file),
        next_suffix(next_suffix), replacements(replacements) {}
};

auto LogBegin(const PipelineActionCtx &ctx) -> std::string;

inline auto LogBeginShort(const std::string &in_file) { return llvm::formatv("[c2pancake] {0}:", in_file); }

inline auto LogBeginShort(const llvm::StringRef in_file) { return llvm::formatv("[c2pancake] {0}:", in_file); }
} // namespace pancake

#endif // C2PANCAKE_UTILS_H