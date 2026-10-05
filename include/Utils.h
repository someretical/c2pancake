#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/AddressSpaces.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/SourceCode.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <any>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace clang::ast_matchers {
AST_MATCHER(Stmt, StmtNotInMacro) {
  auto &sm = Finder->getASTContext().getSourceManager();
  auto location = Node.getBeginLoc();

  return !sm.isMacroBodyExpansion(location) && !sm.isMacroArgExpansion(location);
}

AST_MATCHER(Expr, ExprNotInMacro) {
  auto &sm = Finder->getASTContext().getSourceManager();
  auto location = Node.getExprLoc();

  return !sm.isMacroBodyExpansion(location) && !sm.isMacroArgExpansion(location);
}
} // namespace clang::ast_matchers

namespace pancake {
enum class PointerWidth : uint8_t { None = 0, W32 = 32, W64 = 64 };
}
extern llvm::cl::opt<pancake::PointerWidth> pointer_bits;

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
Each stage receives a fresh StageContext for every attempt. The per-input
PipelineRunCtx owns state and artifacts shared by all stages for that input.
*/
enum class StageControl : uint8_t { Continue, Repeat, NextFile, Abort };
enum class StageKind : uint8_t { Clang, File };

struct StageResult {
  StageControl control = StageControl::Continue;
  bool file_modified = false;
  std::optional<std::string> next_source_file;
  std::vector<std::string> generated_files;
};

inline auto CreateRuntimeError(const std::string &msg, std::source_location loc = std::source_location::current())
    -> llvm::Error;

template <typename T> struct ArtifactKey {
  std::string_view name;
};

class PipelineArtifacts {
  std::unordered_map<std::string, std::any> values;

public:
  template <typename T> auto Set(const ArtifactKey<T> key, T value) -> llvm::Error {
    const auto [it, inserted] = values.try_emplace(std::string(key.name), std::move(value));
    if (!inserted) {
      return CreateRuntimeError(llvm::formatv("Pipeline artifact \"{0}\" was already published", key.name));
    }
    return llvm::Error::success();
  }

  template <typename T> auto Get(const ArtifactKey<T> key) -> T * {
    const auto it = values.find(std::string(key.name));
    return it == values.end() ? nullptr : std::any_cast<T>(&it->second);
  }

  template <typename T> auto Require(const ArtifactKey<T> key) -> llvm::Expected<T &> {
    auto *value = Get(key);
    if (value == nullptr) {
      return CreateRuntimeError(llvm::formatv("Required pipeline artifact \"{0}\" is missing", key.name));
    }
    return *value;
  }

  template <typename T> auto Replace(const ArtifactKey<T> key, T value) -> llvm::Error {
    const auto it = values.find(std::string(key.name));
    if (it == values.end()) {
      return CreateRuntimeError(llvm::formatv("Cannot replace missing pipeline artifact \"{0}\"", key.name));
    }
    if (!std::any_cast<T>(&it->second)) {
      return CreateRuntimeError(llvm::formatv("Pipeline artifact \"{0}\" has an unexpected type", key.name));
    }
    it->second = std::move(value);
    return llvm::Error::success();
  }
};

struct PipelineRunCtx {
  const std::string &initial_file;
  PipelineArtifacts artifacts;
  std::vector<std::string> generated_files;

  explicit PipelineRunCtx(const std::string &initial_file) : initial_file(initial_file) {}
};

struct StageInput {
  PipelineRunCtx &run;
  const size_t stage_index;
  const size_t attempt_index;
  const std::string &current_file;
  const std::string &current_suffix;
  const std::string &next_file;
  const std::string &next_suffix;

  StageInput(PipelineRunCtx &run, size_t stage_index, size_t attempt_index, const std::string &current_file,
             const std::string &current_suffix, const std::string &next_file, const std::string &next_suffix)
      : run(run), stage_index(stage_index), attempt_index(attempt_index), current_file(current_file),
        current_suffix(current_suffix), next_file(next_file), next_suffix(next_suffix) {}
};

struct StageOutput {
  clang::tooling::Replacements replacements;
  std::optional<std::string> stage_name;
  llvm::Error error = llvm::Error::success();
  StageResult result;
  bool control_set = false;

  void SetControl(StageControl control) {
    result.control = control;
    control_set = true;
  }
  auto HasControl() const -> bool { return control_set; }
  void MarkModified() { result.file_modified = true; }
  void SetOutputFile(std::string path) { result.next_source_file = std::move(path); }
  void AddGeneratedFile(std::string path) { result.generated_files.emplace_back(std::move(path)); }
};

struct StageContext : StageInput, StageOutput {
  StageContext(PipelineRunCtx &run, size_t stage_index, size_t attempt_index, const std::string &current_file,
               const std::string &current_suffix, const std::string &next_file, const std::string &next_suffix)
      : StageInput(run, stage_index, attempt_index, current_file, current_suffix, next_file, next_suffix) {}
};

auto PrintLogBegin(llvm::raw_ostream &os, const StageContext &ctx) -> void;

auto PrintLogBeginShort(llvm::raw_ostream &os, llvm::StringRef in_file) -> void;

inline auto CreateRuntimeError(const std::string &msg, const std::source_location loc) -> llvm::Error {
  std::string base;
  llvm::raw_string_ostream os(base);
  os << llvm::formatv("Runtime error:\n    at {0}:{1}:{2}: ", loc.file_name(), loc.line(), loc.column());
  os << msg;
  os.flush();
  return llvm::createStringError(std::move(base), std::make_error_code(std::errc::invalid_argument));
}

inline auto CreateRuntimeError(const llvm::formatv_object_base &&msg,
                               const std::source_location loc = std::source_location::current()) -> llvm::Error {
  std::string base;
  llvm::raw_string_ostream os(base);
  os << llvm::formatv("Runtime error:\n    at {0}:{1}:{2}: ", loc.file_name(), loc.line(), loc.column());
  msg.format(os);
  os.flush();
  return llvm::createStringError(std::move(base), std::make_error_code(std::errc::invalid_argument));
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::CharSourceRange &range, const clang::ASTContext &ctx)
    -> llvm::Error;

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Expr *expr, const clang::ASTContext &ctx)
    -> llvm::Error;

inline auto GetSourceText(const clang::Expr *expr, const clang::ASTContext &ctx) -> llvm::Expected<std::string> {
  std::string s;
  llvm::raw_string_ostream os(s);
  auto err = PrintSourceText(os, expr, ctx);
  if (err) {
    return std::move(err);
  }
  os.flush();
  return s;
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Stmt *stmt, const clang::ASTContext &ctx)
    -> llvm::Error;

inline auto GetSourceText(const clang::Stmt *stmt, const clang::ASTContext &ctx) -> llvm::Expected<std::string> {
  std::string s;
  llvm::raw_string_ostream os(s);
  auto err = PrintSourceText(os, stmt, ctx);
  if (err) {
    return std::move(err);
  }
  os.flush();
  return s;
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::TagDecl *tag_decl, const clang::ASTContext &ctx)
    -> llvm::Error;

inline auto GetSourceText(const clang::TagDecl *tag_decl, const clang::ASTContext &ctx) -> llvm::Expected<std::string> {
  std::string s;
  llvm::raw_string_ostream os(s);
  auto err = PrintSourceText(os, tag_decl, ctx);
  if (err) {
    return std::move(err);
  }
  os.flush();
  return s;
}

inline auto GetPointerWidth(const clang::ASTContext &ctx) -> uint64_t {
  auto width = ctx.getTargetInfo().getPointerWidth(clang::LangAS::Default);
  assert(width == 32 || width == 64);
  return width;
}

inline auto GetWordTypeStr(const clang::ASTContext &ctx) {
  return llvm::formatv("uint{0}_t", pointer_bits != PointerWidth::None ? std::to_underlying(pointer_bits.getValue())
                                                                       : GetPointerWidth(ctx));
}

auto StmtNeedsSemi(const clang::Stmt *s) -> bool;
} // namespace pancake

#endif // C2PANCAKE_UTILS_H