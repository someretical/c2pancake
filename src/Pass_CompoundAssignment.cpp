#include "Pass_CompoundAssignment.h"

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
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::transformer;
using namespace clang::tooling;

namespace pancake::pass_compound_assignment {

namespace {
auto BinaryOpSpelling(BinaryOperatorKind Op) -> std::optional<StringRef> {
  switch (Op) {
  case BO_AddAssign:
    return "+";
  case BO_SubAssign:
    return "-";
  case BO_MulAssign:
    return "*";
  case BO_DivAssign:
    return "/";
  case BO_RemAssign:
    return "%";
  case BO_AndAssign:
    return "&";
  case BO_OrAssign:
    return "|";
  case BO_XorAssign:
    return "^";
  case BO_ShlAssign:
    return "<<";
  case BO_ShrAssign:
    return ">>";
  default:
    return std::nullopt;
  }
}

auto ExprText(const Expr *E, ASTContext &Ctx) -> std::string {
  return getText(CharSourceRange::getTokenRange(E->getSourceRange()), Ctx).str();
}

auto ExprContains(const Expr *haystack, const BinaryOperator *needle) -> bool {
  if (haystack == needle)
    return true;
  for (const Stmt *child : haystack->children()) {
    if (const auto *e = dyn_cast_or_null<Expr>(child))
      if (ExprContains(e, needle))
        return true;
  }
  return false;
}

// Recursively rebuilds a side-effect-free version of `E`. Each
// side-effecting leaf produces:
//   - a bare declaration appended to Decls ("T __cas_tmpN;")
//   - an assignment-expression fragment appended to Inits
//     ("__cas_tmpN = <original-subexpr>")
// and is replaced in the rebuilt text by "__cas_tmpN".
auto Stabilise(const Expr *E, ASTContext &Ctx, std::vector<std::string> &Decls, std::vector<std::string> &Inits,
               int &Counter) -> std::string {
  const Expr *orig = E;
  E = E->IgnoreParenImpCasts();

  if (isa<DeclRefExpr>(E))
    return ExprText(E, Ctx);

  if (const auto *me = dyn_cast<MemberExpr>(E)) {
    std::string const base = Stabilise(me->getBase(), Ctx, Decls, Inits, Counter);
    StringRef arrow = me->isArrow() ? "->" : ".";
    return base + arrow.str() + me->getMemberDecl()->getNameAsString();
  }

  if (const auto *ase = dyn_cast<ArraySubscriptExpr>(E)) {
    std::string const base = Stabilise(ase->getBase(), Ctx, Decls, Inits, Counter);
    std::string const idx = Stabilise(ase->getIdx(), Ctx, Decls, Inits, Counter);
    return base + "[" + idx + "]";
  }

  if (const auto *uo = dyn_cast<UnaryOperator>(E)) {
    if (uo->getOpcode() == UO_Deref) {
      std::string const sub = Stabilise(uo->getSubExpr(), Ctx, Decls, Inits, Counter);
      return "(*(" + sub + "))";
    }
  }

  // Side-effecting (or unrecognized) leaf: declare a temp now, defer
  // its initialization into the comma-expression so it runs at exactly
  // the original evaluation point, not earlier.
  std::string tmp = "__cas_tmp" + std::to_string(Counter++);
  std::string const type_name = orig->getType().getAsString();
  Decls.push_back(type_name + " " + tmp + ";");
  Inits.push_back(tmp + " = " + ExprText(orig, Ctx));
  return tmp;
}

enum class AnchorKind { Direct, NeedsBraceWrap, Unsupported };

struct AnchorResult {
  const Stmt *Anchor;
  AnchorKind Kind;
};

auto FindEnclosingFullStatement(const Stmt *S, ASTContext &Ctx) -> AnchorResult {
  const Stmt *cur = S;

  while (true) {
    DynTypedNodeList const parents = Ctx.getParents(*cur);
    if (parents.empty())
      return {.Anchor = cur, .Kind = AnchorKind::Direct}; // top-level fallback

    const Stmt *p = parents[0].get<Stmt>();
    if (p == nullptr) {
      // Parent is a Decl/TranslationUnitDecl (e.g. Cur is a function's
      // top-level CompoundStmt itself, or this is unreachable in normal
      // expression-statement contexts). Treat as direct.
      return {.Anchor = cur, .Kind = AnchorKind::Direct};
    }

    if (isa<CompoundStmt>(p))
      return {.Anchor = cur, .Kind = AnchorKind::Direct}; // Cur is a plain statement in a block

    if (const auto *If = dyn_cast<IfStmt>(p)) {
      if (cur == If->getThen() || cur == If->getElse())
        return {.Anchor = cur, .Kind = AnchorKind::NeedsBraceWrap};
      // Cur is the condition (or, in C++17, the init-statement, which is
      // itself fine to insert before — but the *condition expression*
      // specifically cannot host a declaration). Walk past the IfStmt.
      cur = p;
      continue;
    }

    if (const auto *w = dyn_cast<WhileStmt>(p)) {
      if (cur == w->getBody())
        return {.Anchor = cur, .Kind = AnchorKind::NeedsBraceWrap};
      cur = p; // in condition
      continue;
    }

    if (const auto *d = dyn_cast<DoStmt>(p)) {
      if (cur == d->getBody())
        return {.Anchor = cur, .Kind = AnchorKind::NeedsBraceWrap};
      cur = p; // in condition
      continue;
    }

    if (const auto *f = dyn_cast<ForStmt>(p)) {
      if (cur == f->getBody())
        return {.Anchor = cur, .Kind = AnchorKind::NeedsBraceWrap};
      // In init/cond/inc: no valid statement slot exists here at all.
      cur = p;
      continue;
    }

    if (const auto *sw = dyn_cast<SwitchStmt>(p)) {
      if (cur == sw->getBody())
        return {.Anchor = cur, .Kind = AnchorKind::Direct}; // body is virtually always a CompoundStmt
      cur = p;                                              // in switch condition
      continue;
    }

    if (isa<CaseStmt>(p) || isa<DefaultStmt>(p) || isa<LabelStmt>(p)) {
      // Declaration immediately after a label is restricted pre-C23 /
      // in some compiler modes. Rather than guess at standard-mode
      // compatibility, refuse and let the caller report why.
      return {.Anchor = p, .Kind = AnchorKind::Unsupported};
    }

    // Any other statement kind we don't specifically reason about
    if (!isa<IfStmt, WhileStmt, DoStmt, ForStmt, SwitchStmt>(p))
      return {.Anchor = p, .Kind = AnchorKind::Unsupported};

    cur = p;
  }
}

auto CompoundAssignRule() -> RewriteRule {
  auto matcher =
      binaryOperator(isAssignmentOperator(), unless(hasOperatorName("=")), hasAncestor(stmt().bind("anchor")))
          .bind("assign");

  return makeRule(matcher, [](const MatchFinder::MatchResult &Result) -> Expected<SmallVector<Edit, 1>> {
    const auto *bo = Result.Nodes.getNodeAs<BinaryOperator>("assign");
    ASTContext &ctx = *Result.Context;

    auto op_spelling = BinaryOpSpelling(bo->getOpcode());
    if (!op_spelling)
      return noEdits()(Result);

    // Suppress inner compound-assignments that are nested in an outer one's LHS.
    // The outer rewrite will handle them via Stabilise(); emitting independent
    // replacements for them too causes overlapping-range conflicts.
    for (DynTypedNode const &par : ctx.getParents(*bo)) {
      if (const auto *outer = par.get<BinaryOperator>()) {
        if (outer->isCompoundAssignmentOp() && ExprContains(outer->getLHS(), bo))
          return noEdits()(Result);
      }
    }

    std::vector<std::string> decls;
    std::vector<std::string> inits;
    int counter = 0;
    std::string stable_lhs = Stabilise(bo->getLHS(), ctx, decls, inits, counter);
    std::string const rhs_text = ExprText(bo->getRHS(), ctx);
    std::string const assign_expr = stable_lhs + " = " + stable_lhs + " " + op_spelling->str() + " (" + rhs_text + ")";

    SmallVector<ASTEdit, 1> edits;

    if (decls.empty()) {
      edits.push_back(changeTo(node("assign"), cat(assign_expr)));
      return editList(std::move(edits))(Result);
    }

    std::string comma_expr = "(";
    for (const auto &i : inits)
      comma_expr += i + ", ";
    comma_expr += assign_expr + ")";

    std::string decl_text;
    for (const auto &d : decls)
      decl_text += d + " ";

    AnchorResult const anchor = FindEnclosingFullStatement(bo, ctx);

    switch (anchor.Kind) {
    case AnchorKind::Unsupported:
      return llvm::make_error<llvm::StringError>("cannot hoist temporaries for this compound assignment: no "
                                                 "valid statement position found near the enclosing "
                                                 "label/case/loop-clause; skipping",
                                                 llvm::inconvertibleErrorCode());

    case AnchorKind::Direct:
      edits.push_back(insertBefore(node("anchor"), cat(decl_text)));
      edits.push_back(changeTo(node("assign"), cat(comma_expr)));
      return editList(std::move(edits))(Result);

    case AnchorKind::NeedsBraceWrap: {
      // Wrap the bare single-statement body in braces ourselves:
      //   if (x) foo();  ->  if (x) { DECL; foo(); }
      edits.push_back(insertBefore(node("anchor"), cat("{ " + decl_text)));
      edits.push_back(insertAfter(node("anchor"), cat(" }")));
      edits.push_back(changeTo(node("assign"), cat(std::move(comma_expr))));
      return editList(std::move(edits))(Result);
    }
    }
    llvm_unreachable("all AnchorKind cases handled");
  });
}
} // namespace

auto Consumer::HandleTranslationUnit(clang::ASTContext &Ctx) -> void {
  std::vector<AtomicChange> changes;
  auto t = Transformer(CompoundAssignRule(), [&changes](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
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
      llvm::cantFail(repls.add(r));
    }
  }
}

} // namespace pancake::pass_compound_assignment