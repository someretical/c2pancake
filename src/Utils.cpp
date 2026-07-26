#include "Utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <utility>
#include <vector>

namespace pancake {
auto PrintLogBegin(llvm::raw_ostream &os, const PipelineStageCtx &ctx) -> void {
  const auto action_name = ctx.action_name ? llvm::StringRef(*ctx.action_name) : llvm::StringRef("UnknownAction");
  os << llvm::formatv("[c2pancake] [{0:02}-{1:02}] {2}: {3}: ", ctx.major_pass_number, ctx.minor_pass_number,
                      ctx.current_file, action_name);
}

auto PrintLogBeginShort(llvm::raw_ostream &os, const llvm::StringRef in_file) -> void {
  os << llvm::formatv("[c2pancake] {0}: ", in_file);
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::CharSourceRange &range, const clang::ASTContext &ctx)
    -> llvm::Error {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  if (range.isInvalid()) {
    return CreateRuntimeError(
        std::move(llvm::formatv("Invalid token range for source range: {0} - {1}", range.getBegin().printToString(sm),
                                range.getEnd().printToString(sm))));
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    return CreateRuntimeError(
        std::move(llvm::formatv("Invalid file range for source range: {0} - {1}", range.getBegin().printToString(sm),
                                range.getEnd().printToString(sm))));
  }     os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
    return llvm::Error::success();
 
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Expr *expr, const clang::ASTContext &ctx)
    -> llvm::Error {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(expr->getSourceRange());
  if (range.isInvalid()) {
    return CreateRuntimeError(
        std::move(llvm::formatv("Invalid token range for source range: {0} - {1}", range.getBegin().printToString(sm),
                                range.getEnd().printToString(sm))));
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    expr->printPretty(os, nullptr, ctx.getPrintingPolicy(), 0, "\n", &ctx);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
  return llvm::Error::success();
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Stmt *stmt, const clang::ASTContext &ctx)
    -> llvm::Error {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(stmt->getSourceRange());
  if (range.isInvalid()) {
    return CreateRuntimeError(
        std::move(llvm::formatv("Invalid token range for source range: {0} - {1}", range.getBegin().printToString(sm),
                                range.getEnd().printToString(sm))));
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    stmt->printPretty(os, nullptr, ctx.getPrintingPolicy(), 0, "\n", &ctx);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
  return llvm::Error::success();
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::TagDecl *tag_decl, const clang::ASTContext &ctx)
    -> llvm::Error {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(tag_decl->getSourceRange());
  if (range.isInvalid()) {
    return CreateRuntimeError(
        std::move(llvm::formatv("Invalid token range for source range: {0} - {1}", range.getBegin().printToString(sm),
                                range.getEnd().printToString(sm))));
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    tag_decl->print(os, ctx.getPrintingPolicy(), 0, false);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
  return llvm::Error::success();
}

auto StmtNeedsSemi(const clang::Stmt *s) -> bool {
  switch (s->getStmtClass()) {
  case clang::Stmt::CompoundStmtClass:
    return false; // ends in '}'

  case clang::Stmt::IfStmtClass: {
    const auto *if_stmt = cast<clang::IfStmt>(s);
    return StmtNeedsSemi((if_stmt->getElse() != nullptr) ? if_stmt->getElse() : if_stmt->getThen());
  }
  case clang::Stmt::SwitchStmtClass:
    return StmtNeedsSemi(cast<clang::SwitchStmt>(s)->getBody());
  case clang::Stmt::WhileStmtClass:
    return StmtNeedsSemi(cast<clang::WhileStmt>(s)->getBody());
  case clang::Stmt::ForStmtClass:
    return StmtNeedsSemi(cast<clang::ForStmt>(s)->getBody());
  case clang::Stmt::LabelStmtClass:
    return StmtNeedsSemi(cast<clang::LabelStmt>(s)->getSubStmt());
  case clang::Stmt::CaseStmtClass:
    return StmtNeedsSemi(cast<clang::CaseStmt>(s)->getSubStmt());
  case clang::Stmt::DefaultStmtClass:
    return StmtNeedsSemi(cast<clang::DefaultStmt>(s)->getSubStmt());

  case clang::Stmt::DoStmtClass: // do ... while (cond) ;
    [[fallthrough]];
  case clang::Stmt::GotoStmtClass:
    [[fallthrough]];
  case clang::Stmt::ContinueStmtClass:
    [[fallthrough]];
  case clang::Stmt::BreakStmtClass:
    [[fallthrough]];
  case clang::Stmt::ReturnStmtClass:
    return true;

  case clang::Stmt::NullStmtClass: // ';' alone
    return false;

  case clang::Stmt::DeclStmtClass:
    // Needs one syntactically, but Clang's DeclStmt::getSourceRange() already includes it
    return false;

  default:
    // Anything else reaching here is an expression used as a statement
    // (BinaryOperator, CallExpr, UnaryOperator, ...)
    return true;
  }
}

auto StagedCompilationDatabase::GetOriginalFilename(llvm::StringRef Filename) const -> llvm::StringRef {
  const auto ref = Filename;
  if (!current_suffix.empty() && ref.ends_with(current_suffix)) {
    return ref.drop_back(current_suffix.size());
  }
  return ref;
}

auto StagedCompilationDatabase::getCompileCommands(llvm::StringRef Filename) const
    -> std::vector<clang::tooling::CompileCommand> {
  auto original_file = GetOriginalFilename(Filename);
  auto commands = base_db.getCompileCommands(original_file);
  std::vector<clang::tooling::CompileCommand> ret_commands;

  for (auto &command : commands) {
    bool found = false;
    if (command.Filename == original_file) {
      command.Filename = Filename;
      found = true;
    }

    for (auto &arg : command.CommandLine) {
      if (arg == original_file) {
        arg = Filename;
      }
    }

    if (found) {
      ret_commands.push_back(std::move(command));
    }
  }

  return ret_commands;
}
} // namespace pancake
