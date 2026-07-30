#ifndef C2PANCAKE_UTILS_H
#define C2PANCAKE_UTILS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/AddressSpaces.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <source_location>
#include <string>
#include <utility>

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
A new ASTFrontendAction is created for each TU for each pass
So there's a 1-1 relationship between PipelineAction and C2PancakePass
The Ctx is created inside of Pipeline::Run and passed by reference to each PipelineAction and then C2PancakePass
*/
enum class WhatsNext : uint8_t { MoveToNextPass, RepeatPass, MoveToNextFile, Abort };
enum class RunResult : uint8_t { Success, RepeatPass, Fail };
struct PipelineStageCtx {
  const size_t major_pass_number;             // provided by Pipeline::Run
  const size_t minor_pass_number;             // provided by Pipeline::Run
  const std::string &current_file;            // provided by Pipeline::Run
  const std::string &current_suffix;          // provided by Pipeline::Run
  const std::string &next_file;               // provided by Pipeline::Run
  const std::string &next_suffix;             // provided by Pipeline::Run
  clang::tooling::Replacements &replacements; // provided by Pipeline::Run, the pass adds to it

  std::optional<std::string> action_name = std::nullopt; // set by the PipelineAction constructor
  std::optional<WhatsNext> whats_next = std::nullopt;    // set by the PipelineAction inside of HandleTranslationUnit
  llvm::Error error = llvm::Error::success();            // set by the pass itself if an error occurs
  llvm::Error end_src_file_action_error =
      llvm::Error::success(); // set by PipelineAction::EndSourceFileAction if an error occurs at that stage. These
                              // errors are always result in an abort...
  bool file_modified = false; // set by EndSourceFileAction if the file was modified by the pass

  explicit PipelineStageCtx(const size_t pass_number, const size_t minor_pass_number,
                            clang::tooling::Replacements &replacements, const std::string &current_file,
                            const std::string &current_suffix, const std::string &next_file,
                            const std::string &next_suffix)
      : major_pass_number(pass_number), minor_pass_number(minor_pass_number), current_file(current_file),
        current_suffix(current_suffix), next_file(next_file), next_suffix(next_suffix), replacements(replacements) {}
};

auto PrintLogBegin(llvm::raw_ostream &os, const PipelineStageCtx &ctx) -> void;

auto PrintLogBeginShort(llvm::raw_ostream &os, llvm::StringRef in_file) -> void;

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