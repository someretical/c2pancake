#include "Pass_GlobalVariablePointerify.h"
#include "Utils.h"

#include <algorithm>
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

namespace {
constexpr static const char *c2pancake_external_global_annotation = "__c2pnk_external_global";
}

namespace clang::ast_matchers {
// Does the declaration carry annotate("<Expected>")?
AST_MATCHER_P(Decl, hasAnnotationString, std::string, Expected) {
  return std::ranges::any_of(Node.specific_attrs<AnnotateAttr>(),
                             [&](const AnnotateAttr *A) -> bool { return A->getAnnotation() == Expected; });
}

// Is this DeclRefExpr the operand of '&' (looking through parentheses)?
AST_MATCHER(DeclRefExpr, isAddrOfOperand) {
  ASTContext &ctx = Finder->getASTContext();
  DynTypedNode cur = DynTypedNode::create(Node);
  while (true) {
    auto parents = ctx.getParents(cur);
    if (parents.empty()) {
      return false;
    }

    const DynTypedNode &p = parents[0];
    if (p.get<ParenExpr>() != nullptr) {
      cur = p;
      continue;
    }

    if (const auto *u = p.get<UnaryOperator>()) {
      return u->getOpcode() == UO_AddrOf;
    }
    return false;
  }
}

// Any global variable carrying the annotation (used for references).
static auto annotatedGlobal() {
  return varDecl(hasGlobalStorage(), hasAnnotationString(c2pancake_external_global_annotation));
}

// The declarations we rewrite: annotated, no initializer, in the main file.
static auto externalGlobalDecl() {
  return varDecl(isExpansionInMainFile(), hasGlobalStorage(), hasAnnotationString(c2pancake_external_global_annotation),
                 unless(hasInitializer(anything())));
}

// "extern int a[10]" -> "extern int (*a_ptr)[10]"
static llvm::Expected<std::string> pointerizedDecl(const MatchFinder::MatchResult &R) {
  const auto *vd = R.Nodes.getNodeAs<VarDecl>("global_var");
  if (!vd)
    return llvm::make_error<llvm::StringError>("global_var not bound", llvm::inconvertibleErrorCode());

  ASTContext &ctx = *R.Context;
  PrintingPolicy pp(ctx.getLangOpts());

  QualType ptr_ty = ctx.getPointerType(vd->getType());
  std::string declarator = vd->getName().str() + "_ptr";
  ptr_ty.getAsStringInternal(declarator, pp); // Declarator is in/out

  std::string out;
  if (vd->getStorageClass() == SC_Extern)
    out = "extern ";
  else if (vd->getStorageClass() == SC_Static)
    out = "static ";
  return out + declarator;
}
} // namespace clang::ast_matchers

namespace pancake::pass_global_variable_pointerify {
namespace {
auto MakeRule() -> RewriteRule {
  /*
  Renames global variables with the __c2pnk_external_global annotation
  - to have a _ptr after their name
  - to be a pointer type instead of the original type
  - rewrite instances of their usage to respect the new pointer type
  */

  // 1. declaration:  extern T x;  ->  extern T *x_ptr;
  auto decl_rule =
      makeRule(externalGlobalDecl().bind("global_var"), changeTo(node("global_var"), run(pointerizedDecl)));

  // 2. &x  ->  x_ptr
  auto addr_rule = makeRule(
      unaryOperator(hasOperatorName("&"), hasUnaryOperand(ignoringParens(
                                              declRefExpr(to(annotatedGlobal()), isExpansionInMainFile()).bind("ref"))))
          .bind("addr"),
      changeTo(node("addr"), cat(name("ref"), "_ptr")));

  // 3. any other use of x  ->  (*x_ptr)
  auto deref_rule =
      makeRule(declRefExpr(to(annotatedGlobal()), isExpansionInMainFile(), unless(isAddrOfOperand())).bind("ref"),
               changeTo(node("ref"), cat("(*", name("ref"), "_ptr)")));

  return applyFirst({decl_rule, addr_rule, deref_rule});
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
  llvm::Error error = llvm::Error::success();
  auto t = Transformer(MakeRule(), [&](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c) {
      changes.insert(changes.end(), c->begin(), c->end());
    } else if (error) {
      error = llvm::joinErrors(std::move(error), c.takeError());
    } else {
      error = c.takeError();
    }
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  if (error) {
    ps_ctx.error = llvm::joinErrors(CreateRuntimeError("Error during transformation"), std::move(error));
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }

  int errors = 0;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto error = ps_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(error));
        errors++;
      }
    }
  }

  if (errors == 0 && changes.empty()) {
    ps_ctx.SetControl(StageControl::Continue);
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.SetControl(StageControl::Repeat);
}
} // namespace pancake::pass_global_variable_pointerify
