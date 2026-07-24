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
auto LogBegin(const PipelineActionCtx &ctx) -> std::string {
  auto action_type = ctx.action_type.value_or(PipelineActionType::None);
  const char *action_type_str{};
  switch (action_type) {
  case PipelineActionType::Rewriter:
    action_type_str = "Rewriter";
    break;
  case PipelineActionType::Analyser:
    action_type_str = "Analyser";
    break;
  default:
    action_type_str = "UnknownActionType";
    break;
  }
  return llvm::formatv("[c2pancake] {0} -> {1}: {2}(i={4:02}) {3}:", ctx.current_file, ctx.next_file, action_type_str,
                       ctx.action_name.value_or("UnknownAction"), ctx.major_pass_number);
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::CharSourceRange &range, const clang::ASTContext &ctx)
    -> void {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  if (range.isInvalid()) {
    llvm::errs() << "Invalid token range for source range: ";
    llvm::errs() << range.getBegin().printToString(sm) << " - " << range.getEnd().printToString(sm) << "\n";
    llvm_unreachable("FATAL");
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    llvm::errs() << "Invalid file range for source range: ";
    llvm::errs() << range.getBegin().printToString(sm) << " - " << range.getEnd().printToString(sm) << "\n";
    llvm_unreachable("FATAL");
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Expr *expr, const clang::ASTContext &ctx) -> void {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(expr->getSourceRange());
  if (range.isInvalid()) {
    llvm::errs() << "Invalid token range for source range: ";
    expr->dump();
    llvm_unreachable("FATAL");
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    expr->printPretty(os, nullptr, ctx.getPrintingPolicy(), 0, "\n", &ctx);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::Stmt *stmt, const clang::ASTContext &ctx) -> void {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(stmt->getSourceRange());
  if (range.isInvalid()) {
    llvm::errs() << "Invalid token range for source range: ";
    stmt->dump();
    llvm_unreachable("FATAL");
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    stmt->printPretty(os, nullptr, ctx.getPrintingPolicy(), 0, "\n", &ctx);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
}

auto PrintSourceText(llvm::raw_string_ostream &os, const clang::TagDecl *tag_decl, const clang::ASTContext &ctx)
    -> void {
  const auto &sm = ctx.getSourceManager();
  const auto &lang_opts = ctx.getLangOpts();

  const auto range = clang::CharSourceRange::getTokenRange(tag_decl->getSourceRange());
  if (range.isInvalid()) {
    llvm::errs() << "Invalid token range for source range: ";
    tag_decl->dump();
    llvm_unreachable("FATAL");
  }

  const auto file_range = clang::Lexer::makeFileCharRange(range, sm, lang_opts);
  if (file_range.isInvalid()) {
    // last ditch attempt (will reach this stage for annoying macros)
    tag_decl->print(os, ctx.getPrintingPolicy(), 0, false);
  } else {
    os << clang::Lexer::getSourceText(file_range, sm, lang_opts);
  }
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
