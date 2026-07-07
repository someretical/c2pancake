#include "Pass_LoopsToWhile.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/SourceCode.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// TODO have a check that runs before everything to find any loops that span ACROSS switch statements because those
// cannot be safely transpiled!

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::transformer;
using namespace clang::tooling;

namespace pancake::pass_normalise_while_loops {
namespace {
const std::string while_bind = "while_stmt";
const std::string cond_bind = "while_cond";
const std::string body_bind = "while_body";

auto MakeRule() -> RewriteRule {
  /*
  Moves while loop conditions into the body of the loop
  */
  return applyFirst(
      {makeRule(whileStmt(anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                          hasBody(compoundStmt(
                                      // Bind the body as a compoundStmt so statements() can extract its interior.
                                      anything())
                                      .bind(body_bind)))
                    .bind(while_bind),
                changeTo(node(while_bind),
                         cat("while (1) {\n", statements(body_bind), "\nif (!(", node(cond_bind), ")) break;", "\n}"))),
       makeRule(whileStmt(anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                          hasBody(
                              // Exclude compoundStmt so Case A takes priority in applyFirst.
                              stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(while_bind),
                changeTo(node(while_bind),
                         cat("while (1) {\n", node(body_bind), ";\nif (!(", node(cond_bind), ")) break;", "\n}")))}

  );
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

  bool add_error_occurred = false;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
        add_error_occurred = true;
      }
    }
  }

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_normalise_while_loops

namespace pancake::pass_process_continue_in_for_loops {
namespace {
auto IsContinueDirectlyInsideFor(const ContinueStmt *CS, const ForStmt *FS, ASTContext &Ctx) {
  const Stmt *cur = CS;
  while (true) {
    auto parents = Ctx.getParents(*cur);
    if (parents.empty())
      return false;

    // Hitting a non-Stmt parent (e.g. FunctionDecl) before finding FS means
    // FS is not actually an ancestor of CS. This should theoretically never happen
    const auto *par = parents[0].get<Stmt>();
    if (par == nullptr)
      return false;

    if (par == static_cast<const Stmt *>(FS))
      return true;

    if (llvm::isa<ForStmt>(par) || llvm::isa<WhileStmt>(par) || llvm::isa<DoStmt>(par))
      return false; // An inner construct owns this continue

    cur = par;
  }
}

auto MakeRule() -> RewriteRule {
  auto inner_continue = continueStmt(hasAncestor(forStmt())).bind("cont_stmt");
  auto matcher =
      forStmt(hasIncrement(stmt().bind("for_inc")), hasBody(stmt(hasDescendant(inner_continue)))).bind("for_loop");

  // reuse the same edit generator for all matches
  auto replacement = edit(changeTo(node("cont_stmt"), cat("{ ", node("for_inc"), "; continue; }")));
  return makeRule(
      matcher,
      [replacement = std::move(replacement)](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
        const auto *fs = result.Nodes.getNodeAs<ForStmt>("for_loop");
        const auto *cs = result.Nodes.getNodeAs<ContinueStmt>("cont_stmt");
        auto empty = llvm::SmallVector<Edit, 1>{};
        if (!fs || !cs)
          return empty;

        if (!IsContinueDirectlyInsideFor(cs, fs, *result.Context))
          return empty;

        return replacement(result);
      });
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
} // namespace pancake::pass_process_continue_in_for_loops

namespace pancake::pass_for_to_while {
namespace {
const std::string for_bind = "for_stmt";
const std::string init_bind = "for_init";
const std::string cond_bind = "for_cond";
const std::string inc_bind = "for_inc";
const std::string body_bind = "for_body";

inline auto OptInit() -> Stencil { return ifBound(init_bind, cat(node(init_bind), "\n"), cat("")); }

inline auto WhileCond() -> Stencil {
  return ifBound(cond_bind, cat("while (", node(cond_bind), ")"), cat("while (1)"));
}

inline auto OptInc() -> Stencil { return ifBound(inc_bind, cat("\n", node(inc_bind), ";"), cat("")); }

auto MakeRule() -> RewriteRule {
  /*
  Rewrites for loops into while loops.

  Structure:
  for (OptInit; OptCond; OptInc) Body
  into
  {
    OptInit
    while (OptCond) {
      Body
      OptInc
    }
  }

  We have 2 cases below since the Body can be a single statement OR a compound statement.
  The compound statement case prevents an extra set of {}s.
  */
  return applyFirst(
      {makeRule(forStmt(anyOf(hasLoopInit(stmt().bind(init_bind)), anything()),
                        anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                        anyOf(hasIncrement(stmt().bind(inc_bind)), anything()),
                        hasBody(compoundStmt(
                                    // Bind the body as a compoundStmt so statements() can extract its interior.
                                    anything())
                                    .bind(body_bind)))
                    .bind(for_bind),
                changeTo(node(for_bind),
                         cat("{\n", OptInit(), WhileCond(), " {", statements(body_bind), OptInc(), "\n}", "\n}"))),
       makeRule(forStmt(anyOf(hasLoopInit(stmt().bind(init_bind)), anything()),
                        anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                        anyOf(hasIncrement(stmt().bind(inc_bind)), anything()),
                        hasBody(
                            // Exclude compoundStmt so Case A takes priority in applyFirst.
                            stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(for_bind),
                changeTo(node(for_bind),
                         cat("{\n", OptInit(), WhileCond(), " {\n", node(body_bind), ";", OptInc(), "\n}", "\n}")))}

  );
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

  bool add_error_occurred = false;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
        add_error_occurred = true;
      }
    }
  }

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_for_to_while

// similar conversion for continue in do-while loops
/*
do {
    body1
    continue;
    body2
} while (cond);

to

{
    while (1) {
        body1
        if (cond) continue;
        body2
        if (!cond) break;
    }
}
*/
namespace pancake::pass_process_continue_in_do_while_loops {
namespace {
auto IsContinueDirectlyInsideDoWhile(const ContinueStmt *CS, const DoStmt *DS, ASTContext &Ctx) {
  const Stmt *cur = CS;
  while (true) {
    auto parents = Ctx.getParents(*cur);
    if (parents.empty())
      return false;

    // Hitting a non-Stmt parent (e.g. FunctionDecl) before finding DS means
    // DS is not actually an ancestor of CS. This should theoretically never happen
    const auto *par = parents[0].get<Stmt>();
    if (par == nullptr)
      return false;

    if (par == static_cast<const Stmt *>(DS))
      return true;

    if (llvm::isa<ForStmt>(par) || llvm::isa<WhileStmt>(par) || llvm::isa<DoStmt>(par))
      return false; // An inner construct owns this continue

    cur = par;
  }
}

auto MakeRule() -> RewriteRule {
  auto inner_continue = continueStmt(hasAncestor(doStmt())).bind("cont_stmt");
  auto matcher =
      doStmt(hasCondition(expr().bind("do_cond")), hasBody(stmt(hasDescendant(inner_continue)))).bind("do_loop");

  // reuse the same edit generator for all matches
  auto replacement = edit(changeTo(node("cont_stmt"), cat("if (", node("do_cond"), ") { continue; }")));
  return makeRule(
      matcher,
      [replacement = std::move(replacement)](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
        const auto *ds = result.Nodes.getNodeAs<DoStmt>("do_loop");
        const auto *cs = result.Nodes.getNodeAs<ContinueStmt>("cont_stmt");
        auto empty = llvm::SmallVector<Edit, 1>{};
        if (!ds || !cs)
          return empty;

        if (!IsContinueDirectlyInsideDoWhile(cs, ds, *result.Context))
          return empty;

        return replacement(result);
      });
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
} // namespace pancake::pass_process_continue_in_do_while_loops

namespace pancake::pass_do_while_to_while {
namespace {
const std::string do_bind = "do_stmt";
const std::string cond_bind = "do_cond";
const std::string body_bind = "do_body";

auto MakeRule() -> RewriteRule {
  /*
  Rewrites do-while loops into while loops.

  Structure:
  do Body while (OptCond);
  into
  {
    while (1) {
      Body
      if (!OptCond) break;
    }
  }
  */
  return applyFirst(
      {makeRule(doStmt(anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                       hasBody(compoundStmt(
                                   // Bind the body as a compoundStmt so statements() can extract its interior.
                                   anything())
                                   .bind(body_bind)))
                    .bind(do_bind),
                changeTo(node(do_bind),
                         cat("while (1) {\n", statements(body_bind), "\nif (!(", node(cond_bind), ")) break;", "\n}"))),
       makeRule(doStmt(anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                       hasBody(
                           // Exclude compoundStmt so Case A takes priority in applyFirst.
                           stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(do_bind),
                changeTo(node(do_bind),
                         cat("while (1) {\n", node(body_bind), ";\nif (!(", node(cond_bind), ")) break;", "\n}")))}

  );
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

  bool add_error_occurred = false;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
        add_error_occurred = true;
      }
    }
  }

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
}; // namespace pancake::pass_do_while_to_while
