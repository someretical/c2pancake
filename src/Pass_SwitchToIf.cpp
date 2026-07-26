#include "Pass_SwitchToIf.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/AttrKinds.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace pancake::pass_add_switch_fallthrough {
namespace {
auto MakeRule() -> RewriteRule {
  auto matcher = caseStmt(isExpansionInMainFile(), hasParent(caseStmt())).bind("case_stmt");
  auto replacement = edit(insertBefore(node("case_stmt"), cat("[[fallthrough]];\n")));
  return makeRule(matcher, replacement);
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
  llvm::Error err = llvm::Error::success();
  auto t = Transformer(MakeRule(), [&](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c)
      changes.insert(changes.end(), c->begin(), c->end());
    else
      err = c.takeError();
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  if (err) {
    ps_ctx.error =
        CreateRuntimeError(llvm::formatv("Error during transformation: {0}", llvm::fmt_consume(std::move(err))));
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = ps_ctx.replacements.add(r)) {
        ps_ctx.error = CreateRuntimeError(llvm::formatv("Add replacement conflict: {0}", err));
        ps_ctx.whats_next = WhatsNext::MoveToNextFile;
        return;
      }
    }
  }

  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_add_switch_fallthrough

namespace pancake::pass_normalise_switches {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner switch statements first, then the outermost one
  static auto shouldTraversePostOrder() -> bool { return true; }

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
        if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() != attr::FallThrough) {
            return false;
          }
          continue; // skip to next statement
        }
        case_body_compound = dyn_cast<CompoundStmt>(case_body);
      } else if (auto *default_stmt = dyn_cast<DefaultStmt>(stmt)) {
        auto *case_body = default_stmt->getSubStmt();
        if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() != attr::FallThrough) {
            return false;
          }
          continue; // skip to next statement
        }
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

  auto RebuildSwitchStmt(SwitchStmt *switchStmt) -> llvm::Error {
    auto *switch_body = dyn_cast<CompoundStmt>(switchStmt->getBody());
    if (switch_body == nullptr) {
      return CreateRuntimeError(llvm::formatv("\n    at {0}\nSwitchStmt body is not a CompoundStmt",
                                              switchStmt->getBeginLoc().printToString(data.Ctx.getSourceManager())));
    }

    struct CaseInfo {
      // The SwitchCase label nodes that head this block
      llvm::SmallVector<SwitchCase *, 4> labels; // this is reverse order of the original source code

      // The body statements belonging to this case block.
      // CAN include the last break/return/continue statement
      llvm::SmallVector<Stmt *, 16> body; // this is reverse order of the original source code
    };
    llvm::SmallVector<CaseInfo, 16> case_infos;

    llvm::SmallVector<Stmt *, 16> const stmt_buf;
    CaseInfo current_case_info;
    /*
    Set to true after we have processed a case statement.
    When adding a new case statement that doesn't end with a terminating statement,
    If this is true, we want to copy the previous case statement's body into the new one first because it's a fall
    through case
    */
    bool just_processed_label = false;
    for (auto it = switch_body->body_rbegin(); it != switch_body->body_rend(); ++it) {
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

        - a AttributedStmt containing a FallThroughAttr - this is done by the pass_add_switch_fallthrough pass
        we add the case statement to the current_case_info
        */

        if (IsTerminatingStmt(case_body)) {
          if (just_processed_label) {
            // current_case_info is completely empty
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          } else {
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          }
        } else if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            // get previous current_case_info and copy this label into it
            if (!case_infos.empty()) {
              auto &prev_case_info = case_infos.back();
              prev_case_info.labels.push_back(case_stmt);
            }
          }
        } else if (auto *compound_stmt = dyn_cast<CompoundStmt>(case_body)) {
          auto *last_stmt = compound_stmt->body_back();
          if (IsTerminatingStmt(last_stmt)) {
            if (just_processed_label) {
              // current_case_info is completely empty
              current_case_info.labels.push_back(case_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            } else {
              current_case_info = CaseInfo{};
              current_case_info.labels.push_back(case_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            }
          } else {
            if (just_processed_label) {
              // current_case_info is completely empty
              current_case_info.labels.push_back(case_stmt);
              if (!case_infos.empty()) {
                const auto &prev_body = case_infos.back().body;
                current_case_info.body.insert(current_case_info.body.end(), prev_body.begin(), prev_body.end());
              }
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            } else {
              current_case_info.labels.push_back(case_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            }
          }
        } else {
          if (just_processed_label) {
            // current_case_info is completely empty
            current_case_info.labels.push_back(case_stmt);
            if (!case_infos.empty()) {
              const auto &prev_body = case_infos.back().body;
              current_case_info.body.insert(current_case_info.body.end(), prev_body.begin(), prev_body.end());
            }
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          } else {
            current_case_info.labels.push_back(case_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          }
        }
        just_processed_label = true;
      } else if (auto *default_stmt = dyn_cast<DefaultStmt>(stmt)) {
        auto *case_body = default_stmt->getSubStmt();

        /*
        default statement is treated the same as a case statement
        */
        if (IsTerminatingStmt(case_body)) {
          if (just_processed_label) {
            // current_case_info is completely empty
            current_case_info.labels.push_back(default_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          } else {
            current_case_info = CaseInfo{};
            current_case_info.labels.push_back(default_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          }
        } else if (auto *attr_stmt = dyn_cast<AttributedStmt>(case_body)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            current_case_info.labels.push_back(default_stmt);
          }
        } else if (auto *compound_stmt = dyn_cast<CompoundStmt>(case_body)) {
          auto *last_stmt = compound_stmt->body_back();
          if (IsTerminatingStmt(last_stmt)) {
            if (just_processed_label) {
              // current_case_info is completely empty
              current_case_info.labels.push_back(default_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            } else {
              current_case_info = CaseInfo{};
              current_case_info.labels.push_back(default_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            }
          } else {
            if (just_processed_label) {
              // current_case_info is completely empty
              current_case_info.labels.push_back(default_stmt);
              if (!case_infos.empty()) {
                const auto &prev_body = case_infos.back().body;
                current_case_info.body.insert(current_case_info.body.end(), prev_body.begin(), prev_body.end());
              }
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            } else {
              current_case_info.labels.push_back(default_stmt);
              current_case_info.body.push_back(case_body);
              case_infos.push_back(current_case_info);
              current_case_info = CaseInfo{};
            }
          }
        } else {
          if (just_processed_label) {
            // current_case_info is completely empty
            current_case_info.labels.push_back(default_stmt);
            if (!case_infos.empty()) {
              const auto &prev_body = case_infos.back().body;
              current_case_info.body.insert(current_case_info.body.end(), prev_body.begin(), prev_body.end());
            }
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          } else {
            current_case_info.labels.push_back(default_stmt);
            current_case_info.body.push_back(case_body);
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          }
        }
        just_processed_label = true;
      } else {
        /*
        - a single terminating statement (break/return/continue)
        append the current_case_info to the vector and make a new CaseInfo with nothing in it
        - a single non-terminating statement/compound statement
        add that statement to the current_case_info
        */

        if (just_processed_label) {
          // if we just processed a case label, then this statement is part of a new case body (but we don't know the
          // label yet...)
          if (!case_infos.empty()) {
            const auto &prev_body = case_infos.back().body;
            current_case_info.body.insert(current_case_info.body.end(), prev_body.begin(), prev_body.end());
          }
          current_case_info.body.push_back(stmt);

          // auto tmp = current_case_info;
          // case_infos.push_back(current_case_info);
          // current_case_info = CaseInfo{};
          // current_case_info.body.insert(current_case_info.body.end(), tmp.body.begin(), tmp.body.end());
          // current_case_info.body.push_back(stmt);
        } else {
          if (IsTerminatingStmt(stmt)) {
            case_infos.push_back(current_case_info);
            current_case_info = CaseInfo{};
          }

          current_case_info.body.push_back(stmt);
        }
        just_processed_label = false;
      }
    }

    case_infos.push_back(current_case_info);
    current_case_info = CaseInfo{};

    auto valid_case_infos = case_infos |
                            std::views::filter([](const CaseInfo &ci) -> bool { return !ci.labels.empty(); }) |
                            std::views::reverse;

    std::string new_body;
    llvm::raw_string_ostream os(new_body);
    for (const auto &case_info : valid_case_infos) {
      auto reverse_labels = case_info.labels | std::views::reverse;
      for (const auto [index, label] : std::views::enumerate(reverse_labels)) {
        os << "\n";

        if (const auto *case_stmt = dyn_cast<CaseStmt>(label)) {
          const auto range = CharSourceRange::getTokenRange(case_stmt->getCaseLoc(), case_stmt->getColonLoc());
          if (auto err = PrintSourceText(os, range, data.Ctx)) {
            return CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nFailed to print source text for case statement: {1}",
                                        case_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                        llvm::fmt_consume(std::move(err)))));
          }
        } else if (const auto *default_stmt = dyn_cast<DefaultStmt>(label)) {
          const auto range = CharSourceRange::getTokenRange(default_stmt->getDefaultLoc(), default_stmt->getColonLoc());
          if (auto err = PrintSourceText(os, range, data.Ctx)) {
            return CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nFailed to print source text for default statement: {1}",
                                        default_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                        llvm::fmt_consume(std::move(err)))));
          }
        }

        if (case_info.body.empty() || index < case_info.labels.size() - 1) {
          os << "[[fallthrough]];\n";
          continue;
        }

        if (case_info.body.size() == 1) {
          if (auto *compound_stmt = dyn_cast<CompoundStmt>(case_info.body[0])) {
            if (compound_stmt->size() > 0 && !IsTerminatingStmt(compound_stmt->body_back())) {
              os << "\n{\n";
              compound_stmt->printPretty(os, nullptr, data.Ctx.getPrintingPolicy());
              os << "\nbreak;\n}";
            } else {
              compound_stmt->printPretty(os, nullptr, data.Ctx.getPrintingPolicy());
            }
            continue;
          }
        }

        os << "\n{\n";
        bool terminating_stmt_found = false;
        const auto body_it = case_info.body | std::views::reverse;
        for (const auto &stmt : body_it) {
          if (auto err = PrintSourceText(os, stmt, data.Ctx)) {
            return CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to print source text for statement: {1}",
                stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
          }
          if (isa<CompoundStmt>(stmt)) {
            os << "\n";
          } else {
            os << ";\n";
          }
          if (IsTerminatingStmt(stmt)) {
            terminating_stmt_found = true;
            break;
          }
        }
        if (!terminating_stmt_found) {
          os << "break;\n";
        }
        os << "}\n";
      }
    }

    // get range between {} of switch body
    const auto range =
        CharSourceRange::getCharRange(switch_body->getLBracLoc().getLocWithOffset(1), switch_body->getRBracLoc());
    os.flush();
    data.replacements.emplace_back(data.Ctx.getSourceManager(), range, new_body, data.Ctx.getLangOpts());
    return llvm::Error::success();
  }

  auto VisitSwitchStmt(SwitchStmt *switchStmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (switchStmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(switchStmt->getBeginLoc()))) {
      return true;
    }

    if (VerifySwitchStmt(switchStmt)) {
      return true; // continue traversing the AST if the switch statement is already valid
    }

    if (auto error = RebuildSwitchStmt(switchStmt)) {
      data.error = std::move(error);
      return false;
    }

    // if there are any outer switch statements, their replacements will conflict so only the innermost switch
    // statement should be rewritten
    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  int errors = 0;
  for (const auto &r : replacements) {
    if (auto err = ps_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      errors++;
    }
  }

  if (errors == 0 && replacements.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_normalise_switches

namespace pancake::pass_switch_to_if {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t switch_cond_tmp_var_counter = 0;
};

struct CaseInfo {
  // The SwitchCase label nodes that head this block
  llvm::SmallVector<SwitchCase *, 4> labels; // this is reverse order of the original source code

  // The body statements belonging to this case block.
  // CAN include the last break/return/continue statement
  llvm::SmallVector<Stmt *, 16> body; // this is reverse order of the original source code
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all inner switch statements first, then the outermost one
  static auto shouldTraversePostOrder() -> bool { return true; }

  auto GetSwitchCondTempVarName() -> auto {
    return llvm::formatv("__c2pnk_switch_cond_tmp_var_{0}_{1}_{2}", data.pa_ctx.major_pass_number,
                         data.pa_ctx.minor_pass_number, data.switch_cond_tmp_var_counter++);
  }

  auto BuildConditionExpr(const CaseInfo &ci, const std::string &switch_cond_var,
                          const bool contains_default_case) const -> auto {
    std::string condition;
    llvm::raw_string_ostream os(condition);

    if (contains_default_case) {
      os << "1UL";
      return condition;
    }

    size_t label_count = 0;
    for (const auto &label : ci.labels | std::views::reverse) {
      if (const auto *case_stmt = dyn_cast<CaseStmt>(label)) {
        if (label_count > 0) {
          os << " || ";
        }
        label_count++;

        os << "(";
        case_stmt->getLHS()->printPretty(os, nullptr, data.Ctx.getPrintingPolicy());
        os << " == ";
        os << switch_cond_var;
        os << ")";
      }
    }

    return os.str();
  }

  auto BuildIfBody(const CaseInfo &ci) const -> Expected<std::string> {
    std::string body;
    llvm::raw_string_ostream os(body);

    if (ci.body.size() != 1) {
      return CreateRuntimeError(llvm::formatv("Case body has {0} statements, expected 1: {1}", ci.body.size()));
    }

    if (const auto *body_stmt = dyn_cast<CompoundStmt>(ci.body.front())) {
      os << "{\n";
      for (const auto &stmt : body_stmt->body()) {
        bool const is_last = (stmt == body_stmt->body_back());
        if (is_last && isa<BreakStmt>(stmt)) {
          // don't output the last break statement
        } else {
          if (auto err = PrintSourceText(os, stmt, data.Ctx)) {
            return CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to print source text for statement: {1}",
                stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
          }
          if (isa<CompoundStmt>(stmt)) {
            os << "\n";
          } else {
            os << ";\n";
          }
        }
      }
      os << "}\n";
    } else {
      return CreateRuntimeError(
          llvm::formatv("Case body is not a compound statement: {0}",
                        ci.body.front()->getBeginLoc().printToString(data.Ctx.getSourceManager())));
    }

    os.flush();
    return body;
  }

  auto ConvertSwitchStmt(SwitchStmt *switchStmt) -> llvm::Error {
    auto &sm = data.Ctx.getSourceManager();
    if (switchStmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(switchStmt->getBeginLoc()))) {
      return llvm::Error::success();
    }

    auto *switch_body = dyn_cast<CompoundStmt>(switchStmt->getBody());
    if (switch_body == nullptr) {
      return llvm::Error::success();
    }

    llvm::SmallVector<CaseInfo, 16> case_infos;

    llvm::SmallVector<Stmt *, 16> const stmt_buf;
    CaseInfo current_case_info{};

    for (auto it = switch_body->body_rbegin(); it != switch_body->body_rend(); ++it) {
      auto *stmt = *it;

      if (auto *case_stmt = dyn_cast<CaseStmt>(stmt)) {
        auto *substmt = case_stmt->getSubStmt();
        if (auto *attr_stmt = dyn_cast<AttributedStmt>(substmt)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            // get previous current_case_info and copy this label into it
            if (!case_infos.empty()) {
              auto &prev_case_info = case_infos.back();
              prev_case_info.labels.push_back(case_stmt);
            }
          }
        } else {
          current_case_info.labels.push_back(case_stmt);
          current_case_info.body.push_back(substmt);
          case_infos.push_back(current_case_info);
          current_case_info = CaseInfo{};
        }
      } else if (auto *default_stmt = dyn_cast<DefaultStmt>(stmt)) {
        auto *substmt = default_stmt->getSubStmt();
        if (auto *attr_stmt = dyn_cast<AttributedStmt>(substmt)) {
          auto attrs = attr_stmt->getAttrs();
          if (attrs.front()->getKind() == attr::FallThrough) {
            // get previous current_case_info and copy this label into it
            if (!case_infos.empty()) {
              auto &prev_case_info = case_infos.back();
              prev_case_info.labels.push_back(default_stmt);
            }
          }
        } else {
          current_case_info.labels.push_back(default_stmt);
          current_case_info.body.push_back(substmt);
          case_infos.push_back(current_case_info);
          current_case_info = CaseInfo{};
        }
      }
    }

    auto valid_case_infos = case_infos |
                            std::views::filter([](const CaseInfo &ci) -> bool { return !ci.labels.empty(); }) |
                            std::views::reverse | std::ranges::to<llvm::SmallVector<CaseInfo, 16>>();
    std::string converted;
    llvm::raw_string_ostream os(converted);

    // first hoist the switch condition into a new tmp var
    auto *switch_cond = switchStmt->getCond();
    auto tmp_var_name = GetSwitchCondTempVarName();
    auto cond_source_text = GetSourceText(switch_cond, data.Ctx);
    if (auto error = cond_source_text.takeError()) {
      return error;
    }
    os << llvm::formatv("{0} {1} = {2};\n", switch_cond->getType().getAsString(), tmp_var_name, *cond_source_text);

    // first case gets turned into an "if"
    // subsequent cases get turned into "else if"
    // if any set of cases has a default, it becomes the last one and is turned into an "else"
    for (const auto &[i, ci] : std::views::enumerate(valid_case_infos)) {
      bool const is_first = (i == 0);
      // bool const is_last = (i + 1 == valid_case_infos.size());
      bool const has_default =
          std::ranges::any_of(ci.labels, [](SwitchCase *sc) -> bool { return isa<DefaultStmt>(sc); });

      if (is_first) {
        if (has_default) {
          // just output the default case body by itself
          auto result = BuildIfBody(ci);
          if (auto error = result.takeError()) {
            return error;
          }
          os << *result;
          break;
        }

        // output the first case as an if statement
        os << "if (";
        os << BuildConditionExpr(ci, tmp_var_name, has_default);
        os << ") ";
        auto result = BuildIfBody(ci);
        if (auto error = result.takeError()) {
          return error;
        }
        os << *result;
      }
      /* else if (is_last) {
        if (has_default) {
          os << "else ";
          os << BuildIfBody(ci);
          break;
        }

        os << "else if (";
        os << BuildConditionExpr(ci, tmp_var_name, has_default);
        os << ") ";
        os << BuildIfBody(ci);
      } */
      else {
        if (has_default) {
          os << "else ";
          auto result = BuildIfBody(ci);
          if (auto error = result.takeError()) {
            return error;
          }
          os << *result;
          break;
        }

        os << "else if (";
        os << BuildConditionExpr(ci, tmp_var_name, has_default);
        os << ") ";
        auto result = BuildIfBody(ci);
        if (auto error = result.takeError()) {
          return error;
        }
        os << *result;
      }
    }

    os.flush();
    data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                   CharSourceRange::getTokenRange(switchStmt->getSourceRange()), converted,
                                   data.Ctx.getLangOpts());
    return llvm::Error::success();
  }

  auto VisitSwitchStmt(SwitchStmt *switchStmt) -> bool {
    if (data.error) {
      return false;
    }

    if (auto error = ConvertSwitchStmt(switchStmt)) {
      data.error = std::move(error);
      return false;
    }

    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = ps_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  int errors = 0;
  for (const auto &r : replacements) {
    if (auto err = ps_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      errors++;
    }
  }

  if (errors == 0 && replacements.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_switch_to_if
