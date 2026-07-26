#include "Pass_LoopsToWhile.h"
#include "Utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <utility>

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
      {makeRule(whileStmt(isExpansionInMainFile(), hasCondition(expr().bind(cond_bind)),
                          unless(hasCondition(ignoringParenImpCasts(integerLiteral(equals(1))))),
                          hasBody(compoundStmt(
                                      // Bind the body as a compoundStmt so statements() can extract its interior.
                                      anything())
                                      .bind(body_bind)))
                    .bind(while_bind),
                changeTo(node(while_bind),
                         cat("while (1UL) {\nif (!(", node(cond_bind), ")) { break; }", statements(body_bind), "\n}"))),
       makeRule(whileStmt(isExpansionInMainFile(), hasCondition(expr().bind(cond_bind)),
                          unless(hasCondition(ignoringParenImpCasts(integerLiteral(equals(1))))),
                          hasBody(
                              // Exclude compoundStmt so Case A takes priority in applyFirst.
                              stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(while_bind),
                changeTo(node(while_bind),
                         cat("while (1UL) {\nif (!(", node(cond_bind), ")) { break; }", node(body_bind), ";\n}")))}

  );
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

  int errors = 0;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = ps_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        errors++;
      }
    }
  }

  if (errors == 0 && changes.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_normalise_while_loops

namespace pancake::pass_process_continue_in_for_loops {
namespace {
auto IsContinueDirectlyInsideFor(const ContinueStmt *CS, const ForStmt *FS, ASTContext &Ctx) {
  const Stmt *cur = CS;
  while (true) {
    auto parents = Ctx.getParentMapContext().getParents(*cur);
    if (parents.empty())
      return false;

    // Hitting a non-Stmt parent (e.g. FunctionDecl) before finding FS means
    // FS is not actually an ancestor of CS. This should theoretically never happen
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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
  auto matcher = forStmt(isExpansionInMainFile(), hasIncrement(stmt().bind("for_inc")),
                         hasBody(stmt(hasDescendant(inner_continue))))
                     .bind("for_loop");

  // reuse the same edit generator for all matches
  auto replacement = edit(changeTo(node("cont_stmt"), cat("{ ", node("for_inc"), "; continue; }")));
  return makeRule(
      matcher,
      [replacement = std::move(replacement)](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
        const auto *fs = result.Nodes.getNodeAs<ForStmt>("for_loop");
        const auto *cs = result.Nodes.getNodeAs<ContinueStmt>("cont_stmt");
        if (!fs || !cs)
          return noEdits()(result);

        if (!IsContinueDirectlyInsideFor(cs, fs, *result.Context))
          return noEdits()(result);

        return replacement(result);
      });
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
} // namespace pancake::pass_process_continue_in_for_loops

namespace pancake::pass_for_to_while {
namespace {
const std::string for_bind = "for_stmt";
const std::string init_bind = "for_init";
const std::string cond_bind = "for_cond";
const std::string inc_bind = "for_inc";
const std::string body_bind = "for_body";

inline auto OptInit() -> Stencil { return ifBound(init_bind, cat(node(init_bind), "\n"), cat("")); }

inline auto BreakCond() -> Stencil {
  return ifBound(cond_bind, cat("if (!(", node(cond_bind), ")) { break; }"), cat(""));
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
    while (1UL) {
      if (!OptCond) {
        break;
      }
      Body
      OptInc
    }
  }

  We have 2 cases below since the Body can be a single statement OR a compound statement.
  The compound statement case prevents an extra set of {}s.
  */
  return applyFirst(
      {makeRule(forStmt(isExpansionInMainFile(), anyOf(hasLoopInit(stmt().bind(init_bind)), anything()),
                        anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                        anyOf(hasIncrement(stmt().bind(inc_bind)), anything()),
                        hasBody(compoundStmt(
                                    // Bind the body as a compoundStmt so statements() can extract its interior.
                                    anything())
                                    .bind(body_bind)))
                    .bind(for_bind),
                changeTo(node(for_bind), cat("{\n", OptInit(), "while (1UL) {", BreakCond(), statements(body_bind),
                                             OptInc(), "\n}", "\n}"))),
       makeRule(forStmt(isExpansionInMainFile(), anyOf(hasLoopInit(stmt().bind(init_bind)), anything()),
                        anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                        anyOf(hasIncrement(stmt().bind(inc_bind)), anything()),
                        hasBody(
                            // Exclude compoundStmt so Case A takes priority in applyFirst.
                            stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(for_bind),
                changeTo(node(for_bind), cat("{\n", OptInit(), "while (1UL) {", BreakCond(), node(body_bind), ";",
                                             OptInc(), "\n}", "\n}")))}

  );
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

  int errors = 0;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = ps_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        errors++;
      }
    }
  }

  if (errors == 0 && changes.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
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
    while (1UL) {
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
    auto parents = Ctx.getParentMapContext().getParents(*cur);
    if (parents.empty())
      return false;

    // Hitting a non-Stmt parent (e.g. FunctionDecl) before finding DS means
    // DS is not actually an ancestor of CS. This should theoretically never happen
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
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
  auto matcher = doStmt(isExpansionInMainFile(), hasCondition(expr().bind("do_cond")),
                        hasBody(stmt(hasDescendant(inner_continue))))
                     .bind("do_loop");

  // reuse the same edit generator for all matches
  auto replacement = edit(changeTo(node("cont_stmt"), cat("if (", node("do_cond"), ") { continue; }")));
  return makeRule(
      matcher,
      [replacement = std::move(replacement)](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
        const auto *ds = result.Nodes.getNodeAs<DoStmt>("do_loop");
        const auto *cs = result.Nodes.getNodeAs<ContinueStmt>("cont_stmt");
        if (!ds || !cs)
          return noEdits()(result);

        if (!IsContinueDirectlyInsideDoWhile(cs, ds, *result.Context))
          return noEdits()(result);

        return replacement(result);
      });
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
    while (1UL) {
      Body
      if (!OptCond) break;
    }
  }
  */
  return applyFirst(
      {makeRule(doStmt(isExpansionInMainFile(), anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                       hasBody(compoundStmt(
                                   // Bind the body as a compoundStmt so statements() can extract its interior.
                                   anything())
                                   .bind(body_bind)))
                    .bind(do_bind),
                changeTo(node(do_bind), cat("while (1UL) {\n", statements(body_bind), "\nif (!(", node(cond_bind),
                                            ")) { break; }", "\n}"))),
       makeRule(doStmt(isExpansionInMainFile(), anyOf(hasCondition(expr().bind(cond_bind)), anything()),
                       hasBody(
                           // Exclude compoundStmt so Case A takes priority in applyFirst.
                           stmt(unless(compoundStmt())).bind(body_bind)))
                    .bind(do_bind),
                changeTo(node(do_bind), cat("while (1UL) {\n", node(body_bind), ";\nif (!(", node(cond_bind),
                                            ")) { break; }", "\n}")))}

  );
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

  int errors = 0;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = ps_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        errors++;
      }
    }
  }

  if (errors == 0 && changes.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
}; // namespace pancake::pass_do_while_to_while
