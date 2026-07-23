#include "Pass_IntegerConversion.h"
#include "Pass_TransformLogicalExpressions.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/ParentMapContext.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <clang/lib/CodeGen/Address.h>
#include <clang/lib/CodeGen/CGRecordLayout.h>
#include <clang/lib/CodeGen/CodeGenFunction.h>
#include <clang/lib/CodeGen/CodeGenModule.h>
#include <clang/lib/CodeGen/CodeGenTypes.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::transformer;
using namespace clang::ast_matchers;

namespace pancake::pass_implicit_to_explicit_casts {
namespace {
class TypeNameComputation : public MatchComputation<std::string> {
public:
  explicit TypeNameComputation(std::string id) : id(std::move(id)) {}

  auto eval(const MatchFinder::MatchResult &result, std::string *out) const -> llvm::Error override {
    const auto *expr = result.Nodes.getNodeAs<Expr>(id);
    if (expr == nullptr) {
      return llvm::make_error<llvm::StringError>(llvm::formatv("TypeName: id '{0}' is not bound to an Expr", id),
                                                 llvm::inconvertibleErrorCode());
    }
    llvm::raw_string_ostream os(*out);
    expr->getType().print(os, result.Context->getPrintingPolicy());
    os.flush();

    return llvm::Error::success();
  }

  auto toString() const -> std::string override { return llvm::formatv("TypeName({0})", id); }

private:
  std::string id;
};

auto TypeName(std::string Id) -> TextGenerator { return std::make_shared<TypeNameComputation>(std::move(Id)); }

auto MakeRule() -> RewriteRule {
  return makeRule(implicitCastExpr(isExpansionInMainFile(), unless(hasCastKind(CK_LValueToRValue)),
                                   unless(hasCastKind(CK_NoOp)), unless(hasCastKind(CK_ArrayToPointerDecay)),
                                   unless(hasCastKind(CK_FunctionToPointerDecay)),
                                   unless(hasCastKind(CK_BuiltinFnToFnPtr)))
                      .bind("cast"),
                  changeTo(node("cast"), cat("(", TypeName("cast"), ")", node("cast"))));
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
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

  pa_ctx.run_result = RunResult::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.run_result = RunResult::Success;
  }
}
} // namespace pancake::pass_implicit_to_explicit_casts

namespace pancake::pass_integer_conversion {
namespace {
const char *signed_i64_helpers = R"(/* c2pancake generated code start: helpers for i64 signed ops */
uint64_t __c2pnk_se_i8_to_u64(int8_t x) {{
    uint64_t y = (uint8_t)x;
    return (y ^ (uint64_t)0x80) - (uint64_t)0x80;
}

uint64_t __c2pnk_se_i16_to_u64(int16_t x) {{
    uint64_t y = (uint16_t)x;
    return (y ^ (uint64_t)0x8000) - (uint64_t)0x8000;
}

uint64_t __c2pnk_se_i32_to_u64(int32_t x) {{
    uint64_t y = (uint32_t)x;
    return (y ^ (uint64_t)0x80000000) - (uint64_t)0x80000000;
}

uint64_t __c2pnk_i64_lt(uint64_t a, uint64_t b) {{
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {{
        return sa > sb;
    } else {{
        return a < b;
    }
}

uint64_t __c2pnk_i64_lte(uint64_t a, uint64_t b) {{
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {{
        return sa > sb;
    } else {{
        return a <= b;
    }
}

uint64_t __c2pnk_i64_gt(uint64_t a, uint64_t b) {{
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {{
        return sa < sb;
    } else {{
        return a > b;
    }
}

uint64_t __c2pnk_i64_gte(uint64_t a, uint64_t b) {{
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {{
        return sa < sb;
    } else {{
        return a >= b;
    }
}

/* arithmetic right shift */
uint64_t __c2pnk_i64_sar(uint64_t a, uint64_t n) {{
    assert(n > 0);
    assert(n < 64);

    uint64_t sign = a >> 63;
    uint64_t mask = 0 - sign;
    uint64_t fill = mask << (64 - n);

    return (a >> n) | fill;
}
/* c2pancake generated code end: helpers for i64 signed ops */

)";

const char *signed_i32_helpers = R"(/* c2pancake generated code start: helpers for i32 signed ops */
uint32_t __c2pnk_se_i8_to_u32(int8_t x) {{
    uint32_t y = (uint8_t)x;
    return (y ^ (uint32_t)0x80) - (uint32_t)0x80;
}

uint32_t __c2pnk_se_i16_to_u32(int16_t x) {{
    uint32_t y = (uint16_t)x;
    return (y ^ (uint32_t)0x8000) - (uint32_t)0x8000;
}

uint32_t __c2pnk_i32_lt(uint32_t a, uint32_t b) {{
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {{
        return sa > sb;
    } else {{
        return a < b;
    }
}

uint32_t __c2pnk_i32_lte(uint32_t a, uint32_t b) {{
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {{
        return sa > sb;
    } else {{
        return a <= b;
    }
}

uint32_t __c2pnk_i32_gt(uint32_t a, uint32_t b) {{
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {{
        return sa < sb;
    } else {{
        return a > b;
    }
}

uint32_t __c2pnk_i32_gte(uint32_t a, uint32_t b) {{
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {{
        return sa < sb;
    } else {{
        return a >= b;
    }
}

uint32_t __c2pnk_i32_sar(uint32_t a, uint32_t n) {{
    assert(n > 0);
    assert(n < 32);

    uint32_t sign = a >> 31;
    uint32_t mask = 0 - sign;
    uint32_t fill = mask << (32 - n);

    return (a >> n) | fill;
}
/* c2pancake generated code end: helpers for i32 signed ops */

)";

struct WorkerData {
  ASTContext &Ctx;
  PipelineActionCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  size_t tmp_var_counter = 0;
  bool need_int_helpers = false;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.pa_ctx.major_pass_number, data.pa_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto PrintType(const QualType ty, const llvm::StringRef var_name) const -> std::string {
    std::string s;
    llvm::raw_string_ostream os(s);
    ty.print(os, data.Ctx.getPrintingPolicy(), var_name);
    return os.str();
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = pa_ctx, .replacements = replacements, .need_int_helpers = false};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.need_int_helpers) {
    replacements.emplace_back(
        Ctx.getSourceManager(), Ctx.getSourceManager().getLocForStartOfFile(Ctx.getSourceManager().getMainFileID()), 0,
        llvm::formatv(GetPointerWidth(Ctx) == 32 ? signed_i32_helpers : signed_i64_helpers).str());
  }

  bool add_error_occurred = false;
  for (const auto &r : replacements) {
    if (auto err = pa_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
      add_error_occurred = true;
    }
  }

  pa_ctx.run_result = RunResult::Success;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.run_result = RunResult::Success;
  }
}
} // namespace pancake::pass_integer_conversion