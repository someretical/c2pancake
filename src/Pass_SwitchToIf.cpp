#include "Pass_SwitchToIf.h"

#include <cassert>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/SourceCode.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace pancake::normalise_switches {
namespace {
auto MakeRule() -> RewriteRule {
  auto matcher = caseStmt(hasParent(caseStmt().bind("case_stmt")));
  auto replacement = edit(insertBefore(node("case_stmt"), cat("[[fallthrough]];")));
  return makeRule(matcher, replacement);
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  std::vector<AtomicChange> changes;
  auto t = Transformer(MakeRule(), [&changes](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c)
      changes.insert(changes.end(), c->begin(), c->end());
    else
      llvm::consumeError(c.takeError());
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::reportFatalInternalError(
            llvm::formatv("Failed to add replacement: {0}", llvm::fmt_consume(std::move(err))));
      }
    }
  }

  // this pass should only be executed once!
  pa_ctx.failure_mode = FailureMode::Success;
}
} // namespace pancake::normalise_switches

namespace pancake::pass_switch_to_if {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  std::vector<Replacement> &replacements;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner switch statements first, then the outermost one
  auto shouldTraversePostOrder() -> bool { return true; }

  static auto VerifySwitchStmt(SwitchStmt *switchStmt) -> bool {
    // verify if switch statement needs rewriting
    // basically check that every case is followed by a compound statement which ends in either a break/return/continue
    // statement and there are no random statements in between the case labels and the compound statement this should be
    // a compound statement
    auto *compound_stmt = cast<CompoundStmt>(switchStmt->getBody());
    if (compound_stmt == nullptr) {
      return false;
    }

    for (auto *stmt : compound_stmt->body()) {
      CompoundStmt *case_body_compound = nullptr;

      if (auto *case_stmt = dyn_cast<CaseStmt>(stmt)) {
        auto *case_body = case_stmt->getSubStmt();
        case_body_compound = dyn_cast<CompoundStmt>(case_body);
      } else if (auto *default_stmt = dyn_cast<DefaultStmt>(stmt)) {
        auto *case_body = default_stmt->getSubStmt();
        case_body_compound = dyn_cast<CompoundStmt>(case_body);
      } else {
        return false;
      }

      if (case_body_compound == nullptr) {
        return false;
      }
      if (case_body_compound->body().empty()) {
        return false;
      }
      auto *last_stmt = case_body_compound->body_back();
      if (!isa<BreakStmt>(last_stmt) && !isa<ReturnStmt>(last_stmt) && !isa<ContinueStmt>(last_stmt)) {
        return false;
      }
    }

    return true;
  }

  static auto IsTerminatingStmt(Stmt *stmt) -> bool {
    return isa<BreakStmt>(stmt) || isa<ReturnStmt>(stmt) || isa<ContinueStmt>(stmt);
  }

  auto RebuildSwitchStmt(SwitchStmt *switchStmt) -> void {
    std::string replacement_text{};
    llvm::raw_string_ostream os(replacement_text);

    auto *body = dyn_cast<CompoundStmt>(switchStmt->getBody());
    if (body == nullptr) {
      return;
    }

    struct CaseInfo {
      // The SwitchCase label nodes that head this block
      llvm::SmallVector<SwitchCase *, 4> labels; // this is reverse order of the original source code

      // The body statements belonging to this case block.
      // CAN include the last break/return/continue statement
      std::vector<Stmt *> body; // this is reverse order of the original source code
    };
    std::vector<CaseInfo> case_infos;

    std::vector<Stmt *> stmt_buf;
    CaseInfo current_case_info;
    /*
    Set to true after we have processed a case statement.
    When adding a new case statement that doesn't end with a terminating statement,
    If this is true, we want to copy the previous case statement's body into the new one first because it's a fall
    through case
    */
    for (auto it = body->body_rbegin(); it != body->body_rend(); ++it) {
      auto *stmt = *it;

      if (auto *case_stmt = dyn_cast<CaseStmt>(stmt)) {
        auto *case_body = case_stmt->getSubStmt();
        /*
        case body can be:
        - a single terminating statement (break/return/continue)
        discard the current_case_info and make a new CaseInfo with nothing in it
        - a single non-terminating statement
        add that statement to the current_case_info and push it to the vector and start a new one
        - a compound statement
        we need to check if the last statement is a break/return/continue statement
          - if it is, make a new CaseInfo with just this case label and the compound statement as the body and anything
          in current_case_info is just discarded because it is unreachable
          - if it isn't,
            1. push the current_case_info
            2. make a new CaseInfo with this case label and copy the previous current_case_info's body into it
            3. add the compound statement to the new CaseInfo's body and push it to the vector

        - a AttributedStmt containing a FallThroughAttr - this is done by the normalise_switches pass
        we add the case statement to the current_case_info
        */

        if (IsTerminatingStmt(case_body)) {
          case_infos.push_back(current_case_info);
          current_case_info = CaseInfo{};
          current_case_info.labels.push_back(case_stmt);
          current_case_info.body.push_back(case_body);
        } else if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            current_case_info.labels.push_back(case_stmt);
          }
        } else if (auto *compound_stmt = dyn_cast<CompoundStmt>(case_body)) {
          auto *last_stmt = compound_stmt->body_back();
          if (IsTerminatingStmt(last_stmt)) {
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.push_back(compound_stmt);
          } else {
            auto kept_stmts = current_case_info.body;
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.insert(current_case_info.body.end(), kept_stmts.begin(), kept_stmts.end());
            current_case_info.body.push_back(compound_stmt);
          }
        } else {
          current_case_info.labels.push_back(case_stmt);
          current_case_info.body.push_back(case_body);
        }
      } else if (auto *default_stmt = dyn_cast<DefaultStmt>(stmt)) {
        auto *case_body = default_stmt->getSubStmt();

        /*
        default statement is treated the same as a case statement
        */
        if (IsTerminatingStmt(case_body)) {
          case_infos.push_back(current_case_info);
          current_case_info = CaseInfo{};
          current_case_info.labels.push_back(case_stmt);
          current_case_info.body.push_back(case_body);
        } else if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            current_case_info.labels.push_back(case_stmt);
          }
        } else if (auto *compound_stmt = dyn_cast<CompoundStmt>(case_body)) {
          auto *last_stmt = compound_stmt->body_back();
          if (IsTerminatingStmt(last_stmt)) {
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.push_back(compound_stmt);
          } else {
            auto kept_stmts = current_case_info.body;
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.insert(current_case_info.body.end(), kept_stmts.begin(), kept_stmts.end());
            current_case_info.body.push_back(compound_stmt);
          }
        } else {
          current_case_info.labels.push_back(case_stmt);
          current_case_info.body.push_back(case_body);
        }
      } else {
        /*
        - a single terminating statement (break/return/continue)
        append the current_case_info to the vector and make a new CaseInfo with nothing in it
        - a single non-terminating statement/compound statement
        add that statement to the current_case_info
        */

        if (IsTerminatingStmt(stmt)) {
          case_infos.push_back(current_case_info);
          current_case_info = CaseInfo{};
        } else {
          current_case_info.body.push_back(stmt);
        }
      }
    }

    auto valid_case_infos = case_infos | std::views::filter([](const CaseInfo &ci) { return !ci.labels.empty(); });

    for (const auto &case_info : valid_case_infos) {
      for (const auto *label : case_info.labels) {
        if (auto *case_stmt = dyn_cast<CaseStmt>(label)) {
          os << "if (" << switchStmt->getCond()->IgnoreImpCasts()->IgnoreParens()->getStmtClassName()
             << " == " << case_stmt->getLHS()->IgnoreImpCasts()->IgnoreParens()->getStmtClassName() << ") {\n";
        } else if (auto *default_stmt = dyn_cast<DefaultStmt>(label)) {
          os << "else {\n";
        }
      }

      for (const auto *stmt : case_info.body) {
        stmt->printPretty(os, nullptr, data.Ctx.getPrintingPolicy());
        os << "\n";
      }

      os << "}\n";
    }

    // data.replacements.emplace_back(data.Ctx.getSourceManager(), switchStmt, replacement_text,
    // data.Ctx.getLangOpts());
  }

  auto VisitSwitchStmt(SwitchStmt *switchStmt) -> bool {
    if (!VerifySwitchStmt(switchStmt)) {
      return true; // continue traversing the AST
    }

    RebuildSwitchStmt(switchStmt);

    // if there are any outer switch statements, their replacements will conflict so only the innermost switch statement
    // should be rewritten
    return true;
  }
};

} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  std::vector<Replacement> replacements;
  WorkerData data{.Ctx = Ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  bool add_error_occurred = false;
  for (const auto &r : replacements) {
    if (auto err = pa_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
      add_error_occurred = true;
    }
  }

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}

} // namespace pancake::pass_switch_to_if
