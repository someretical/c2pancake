#include "Pass_IntegerConversion.h"
#include "Pass_TransformLogicalExpressions.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Transformer/MatchConsumer.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <clang/lib/CodeGen/CodeGenFunction.h>
#include <clang/lib/CodeGen/CodeGenModule.h>
#include <clang/lib/CodeGen/CodeGenTypes.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <memory>
#include <optional>
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
                  changeTo(node("cast"), cat("(", TypeName("cast"), ")(", node("cast"), ")")));
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
  llvm::Error error = llvm::Error::success();
  auto t = Transformer(MakeRule(), [&](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c)
      changes.insert(changes.end(), c->begin(), c->end());
    else
      error = c.takeError();
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  if (error) {
    ps_ctx.error =
        CreateRuntimeError(llvm::formatv("Error during transformation: {0}", llvm::fmt_consume(std::move(error))));
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
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
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_implicit_to_explicit_casts

/*
Every value of every integer type u8, i8, u16, i16, ..., i64 is represented as a single uint64_t, but what that
uint64_t looks like depends on signedness:

- Unsigned N-bit value: stored as its plain zero-extended bit pattern in the low N bits, upper bits 0.
- Signed N-bit value: stored sign-extended to the full 64 bit, i.e. the canonical form is "as if it were already cast to
int64_t".

So the "type" of a uint64_t value in this system isn't encoded in the bits themselves, and the operations (conversions,
comparisons, shifts) are the things responsible for maintaining the correct bit-pattern invariant for whichever type
it's supposed to represent at that point.

This is because pancake only supports word sized variables on the stack.
*/
namespace pancake::pass_integer_conversion {
namespace {
const char *signed_i64_helpers = R"(#include <stdint.h>
/* c2pancake generated code start: helpers for i64 signed ops */

static inline uint64_t __c2pnk_u8_to_u64(uint8_t x) {
    // noop
    return (uint64_t)x;
}

static inline uint64_t __c2pnk_u16_to_u64(uint16_t x) {
    // noop
    return (uint64_t)x;
}

static inline uint64_t __c2pnk_u32_to_u64(uint32_t x) {
    // noop
    return (uint64_t)x;
}

static inline uint64_t __c2pnk_u64_to_u64(uint64_t x) {
    // noop
    return x;
}

static inline uint64_t __c2pnk_i8_to_u64(int8_t x) {
    uint64_t y = (uint64_t)(uint8_t)x;
    return (y ^ 0x80UL) - 0x80UL;
}

static inline uint64_t __c2pnk_i16_to_u64(int16_t x) {
    uint64_t y = (uint64_t)(uint16_t)x;
    return (y ^ 0x8000UL) - 0x8000UL;
}

static inline uint64_t __c2pnk_i32_to_u64(int32_t x) {
    uint64_t y = (uint64_t)(uint32_t)x;
    return (y ^ 0x80000000UL) - 0x80000000UL;
}

static inline uint64_t __c2pnk_i64_to_u64(int64_t x) {
    // noop
    return (uint64_t)x;
}

static inline uint64_t __c2pnk_i64_lt(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63UL;
    uint64_t sb = b >> 63UL;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a < b;
    }
}

static inline uint64_t __c2pnk_i64_lte(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63UL;
    uint64_t sb = b >> 63UL;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a <= b;
    }
}

static inline uint64_t __c2pnk_i64_gt(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63UL;
    uint64_t sb = b >> 63UL;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a > b;
    }
}

static inline uint64_t __c2pnk_i64_gte(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63UL;
    uint64_t sb = b >> 63UL;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a >= b;
    }
}

/* arithmetic right shift */
static inline uint64_t __c2pnk_i64_sar(uint64_t a, uint64_t n) {
    uint64_t sign = a >> 63UL;
    uint64_t mask = 0UL - sign;
    uint64_t fill = mask << (64UL - n);

    return (a >> n) | fill;
}

static uint64_t __c2pnk_u64_div(uint64_t dividend, uint64_t divisor) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint64_t quotient = 0UL;
    uint64_t remainder = 0UL;
    uint64_t i = 63UL;

    while (i != UINT64_MAX) {
        remainder = (remainder << 1UL) | ((dividend >> i) & 1UL);
        if (remainder >= divisor) {
            remainder = remainder - divisor;
            quotient = quotient | (1UL << i);
        }
        i = i - 1UL;
    }

    return quotient;
}

static uint64_t __c2pnk_u64_rem(uint64_t dividend, uint64_t divisor) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint64_t remainder = 0UL;
    uint64_t i = 63UL;

    while (i != UINT64_MAX) {
        remainder = (remainder << 1UL) | ((dividend >> i) & 1UL);
        if (remainder >= divisor) {
            remainder = remainder - divisor;
        }
        i = i - 1UL;
    }

    return remainder;
}

static uint64_t __c2pnk_i64_div(uint64_t dividend, uint64_t divisor, uint64_t width) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint64_t int_min_n = 0UL - (1UL << (width - 1UL));
    uint64_t neg_one = ~0UL;

    if (dividend == int_min_n) {
        if (divisor == neg_one) {
            return int_min_n; // N-bit overflow: wraps back to INT_MIN(N)
        }
    }

    uint64_t dividend_sign = dividend >> 63UL;
    uint64_t divisor_sign = divisor >> 63UL;
    uint64_t quotient_sign = dividend_sign ^ divisor_sign;

    uint64_t abs_dividend;
    if (dividend_sign) {
        abs_dividend = 0UL - dividend;
    } else {
        abs_dividend = dividend;
    }

    uint64_t abs_divisor;
    if (divisor_sign) {
        abs_divisor = 0UL - divisor;
    } else {
        abs_divisor = divisor;
    }

    uint64_t uq = __c2pnk_u64_div(abs_dividend, abs_divisor);

    uint64_t quotient;
    if (quotient_sign) {
        quotient = 0UL - uq;
    } else {
        quotient = uq;
    }

    return quotient;
}

static uint64_t __c2pnk_i64_rem(uint64_t dividend, uint64_t divisor, uint64_t width) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint64_t int_min_n = 0UL - (1UL << (width - 1));
    uint64_t neg_one = ~0UL;

    if (dividend == int_min_n) {
        if (divisor == neg_one) {
            return 0UL; // N-bit overflow: remainder is 0 (exact division)
        }
    }

    uint64_t dividend_sign = dividend >> 63UL;
    uint64_t divisor_sign = divisor >> 63UL;

    uint64_t abs_dividend;
    if (dividend_sign) {
        abs_dividend = 0UL - dividend;
    } else {
        abs_dividend = dividend;
    }

    uint64_t abs_divisor;
    if (divisor_sign) {
        abs_divisor = 0UL - divisor;
    } else {
        abs_divisor = divisor;
    }

    uint64_t ur = __c2pnk_u64_rem(abs_dividend, abs_divisor);

    uint64_t remainder;
    if (dividend_sign) {
        remainder = 0UL - ur;
    } else {
        remainder = ur;
    }

    return remainder;
}

static inline uint64_t __c2pnk_trunc_u64_to_u64(uint64_t x) {
    // noop
    return x;
}

static inline uint64_t __c2pnk_trunc_u64_to_u32(uint64_t x) {
    return x & 0xffffffffUL;
}

static inline uint64_t __c2pnk_trunc_u64_to_u16(uint64_t x) {
    return x & 0xffffUL;
}

static inline uint64_t __c2pnk_trunc_u64_to_u8(uint64_t x) {
    return x & 0xffUL;
}

static inline uint64_t __c2pnk_trunc_u64_to_i64(uint64_t x) {
    // noop
    return x;
}

static inline uint64_t __c2pnk_trunc_u64_to_i32(uint64_t x) {
  uint64_t y = x & 0xffffffffUL;
  return (y ^ 0x80000000UL) - 0x80000000UL;
}

static inline uint64_t __c2pnk_trunc_u64_to_i16(uint64_t x) {
  uint64_t y = x & 0xffffUL;
  return (y ^ 0x8000UL) - 0x8000UL;
}

static inline uint64_t __c2pnk_trunc_u64_to_i8(uint64_t x) {
  uint64_t y = x & 0xffUL;
  return (y ^ 0x80UL) - 0x80UL;
}
/* c2pancake generated code end: helpers for i64 signed ops */

)";

const char *signed_i32_helpers = R"(#include <stdint.h>
/* c2pancake generated code start: helpers for i32 signed ops */
static inline uint32_t __c2pnk_i8_to_u32(int8_t x) {
    uint32_t y = (uint32_t)(uint8_t)x;
    return (y ^ 0x80UL) - 0x80UL;
}

static inline uint32_t __c2pnk_i16_to_u32(int16_t x) {
    uint32_t y = (uint32_t)(uint16_t)x;
    return (y ^ 0x8000UL) - 0x8000UL;
}

static inline uint32_t __c2pnk_i32_to_u32(int32_t x) {
    // noop
    return (uint32_t)x;
}

static inline uint32_t __c2pnk_u8_to_u32(uint8_t x) {
    // noop
    return (uint32_t)x;
}

static inline uint32_t __c2pnk_u16_to_u32(uint16_t x) {
    // noop
    return (uint32_t)x;
}

static inline uint32_t __c2pnk_u32_to_u32(uint32_t x) {
    // noop
    return x;
}

static inline uint32_t __c2pnk_i32_lt(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31UL;
    uint32_t sb = b >> 31UL;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a < b;
    }
}

static inline uint32_t __c2pnk_i32_lte(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31UL;
    uint32_t sb = b >> 31UL;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a <= b;
    }
}

static inline uint32_t __c2pnk_i32_gt(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31UL;
    uint32_t sb = b >> 31UL;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a > b;
    }
}

static inline uint32_t __c2pnk_i32_gte(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31UL;
    uint32_t sb = b >> 31UL;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a >= b;
    }
}

static inline uint32_t __c2pnk_i32_sar(uint32_t a, uint32_t n) {
    uint32_t sign = a >> 31UL;
    uint32_t mask = 0UL - sign;
    uint32_t fill = mask << (32UL - n);

    return (a >> n) | fill;
}

static uint32_t __c2pnk_u32_div(uint32_t dividend, uint32_t divisor) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint32_t quotient = 0UL;
    uint32_t remainder = 0UL;
    uint32_t i = 31UL;

    while (i != UINT32_MAX) {
        remainder = (remainder << 1UL) | ((dividend >> i) & 1UL);
        if (remainder >= divisor) {
            remainder = remainder - divisor;
            quotient = quotient | (1UL << i);
        }
        i = i - 1UL;
    }

    return quotient;
}

static uint32_t __c2pnk_u32_rem(uint32_t dividend, uint32_t divisor) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint32_t remainder = 0UL;
    uint32_t i = 31UL;

    while (i != UINT32_MAX) {
        remainder = (remainder << 1UL) | ((dividend >> i) & 1UL);
        if (remainder >= divisor) {
            remainder = remainder - divisor;
        }
        i = i - 1UL;
    }

    return remainder;
}

static uint32_t __c2pnk_i32_div(uint32_t dividend, uint32_t divisor, uint32_t width) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint32_t int_min_n = 0UL - (1UL << (width - 1UL));
    uint32_t neg_one = ~0UL;

    if (dividend == int_min_n) {
        if (divisor == neg_one) {
            return int_min_n; // N-bit overflow: wraps back to INT_MIN(N)
        }
    }

    uint32_t dividend_sign = dividend >> 31UL;
    uint32_t divisor_sign = divisor >> 31UL;
    uint32_t quotient_sign = dividend_sign ^ divisor_sign;

    uint32_t abs_dividend;
    if (dividend_sign) {
        abs_dividend = 0UL - dividend;
    } else {
        abs_dividend = dividend;
    }

    uint32_t abs_divisor;
    if (divisor_sign) {
        abs_divisor = 0UL - divisor;
    } else {
        abs_divisor = divisor;
    }

    uint32_t uq = __c2pnk_u32_div(abs_dividend, abs_divisor);

    uint32_t quotient;
    if (quotient_sign) {
        quotient = 0UL - uq;
    } else {
        quotient = uq;
    }

    return quotient;
}

static uint32_t __c2pnk_i32_rem(uint32_t dividend, uint32_t divisor, uint32_t width) {
    if (divisor == 0UL) {
        return 0UL; // uh oh
    }

    uint32_t int_min_n = 0UL - (1UL << (width - 1UL));
    uint32_t neg_one = ~0UL;

    if (dividend == int_min_n) {
        if (divisor == neg_one) {
            return 0UL; // N-bit overflow: remainder is 0 (exact division)
        }
    }

    uint32_t dividend_sign = dividend >> 31UL;
    uint32_t divisor_sign = divisor >> 31UL;

    uint32_t abs_dividend;
    if (dividend_sign) {
        abs_dividend = 0UL - dividend;
    } else {
        abs_dividend = dividend;
    }

    uint32_t abs_divisor;
    if (divisor_sign) {
        abs_divisor = 0UL - divisor;
    } else {
        abs_divisor = divisor;
    }

    uint32_t ur = __c2pnk_u32_rem(abs_dividend, abs_divisor);

    uint32_t remainder;
    if (dividend_sign) {
        remainder = 0UL - ur;
    } else {
        remainder = ur;
    }

    return remainder;
}

static inline uint32_t __c2pnk_trunc_u32_to_u32(uint32_t x) {
    // noop
    return x;
}

static inline uint32_t __c2pnk_trunc_u32_to_u16(uint32_t x) {
    return x & 0xffffUL;
}

static inline uint32_t __c2pnk_trunc_u32_to_u8(uint32_t x) {
    return x & 0xffUL;
}

static inline uint32_t __c2pnk_trunc_u32_to_i32(uint32_t x) {
    // noop
    return (int32_t)x;
}

static inline uint32_t __c2pnk_trunc_u32_to_i16(uint32_t x) {
    uint32_t y = x & 0xffffUL;
    return (y ^ 0x8000UL) - 0x8000UL;
}

static inline uint32_t __c2pnk_trunc_u32_to_i8(uint32_t x) {
    uint32_t y = x & 0xffUL;
    return (y ^ 0x80UL) - 0x80UL;
}
/* c2pancake generated code end: helpers for i32 signed ops */

)";

struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  size_t tmp_var_counter = 0;
  llvm::Error error = llvm::Error::success();
  bool need_int_helpers = false;
  FunctionDecl *current_function_decl = nullptr;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  static auto shouldTraversePostOrder() -> bool { return false; }

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

  auto VerifyTypeWidth(const QualType type) const -> llvm::Error {
    auto width = data.Ctx.getTypeSize(type);
    if (GetPointerWidth(data.Ctx) == 32) {
      if (width == 32 || width == 16 || width == 8) {
        return llvm::Error::success();
      }
      return CreateRuntimeError(std::move(llvm::formatv("Unsupported type width for 32-bit target: {0}", width)));
    }
    if (GetPointerWidth(data.Ctx) == 64) {
      if (width == 64 || width == 32 || width == 16 || width == 8) {
        return llvm::Error::success();
      }
      return CreateRuntimeError(std::move(llvm::formatv("Unsupported type width for 64-bit target: {0}", width)));
    }
    return CreateRuntimeError(std::move(llvm::formatv("Unsupported pointer width: {0}", GetPointerWidth(data.Ctx))));
  }

  static auto BinOpTypeToStr(const BinaryOperatorKind op) -> Expected<StringRef> {
    switch (op) {
    case BO_LT:
      return "lt";
    case BO_GT:
      return "gt";
    case BO_LE:
      return "lte";
    case BO_GE:
      return "gte";
    case BO_Div:
      return "div";
    case BO_Rem:
      return "rem";
    default:
      return CreateRuntimeError(std::move(llvm::formatv("Unsupported binary operator: {0}", op)));
    }
  }

  using BuiltExpr = pancake::pass_lower_nested_expressions::BuiltExpr;
  using BuildExprCtx = pancake::pass_lower_nested_expressions::BuildExprCtx;
  using Usage = pancake::pass_lower_nested_expressions::Usage;
  template <typename... Args>
  auto GetUsage(Args &&...args)
      -> decltype(pancake::pass_lower_nested_expressions::GetUsage(std::forward<Args>(args)...)) {
    return pancake::pass_lower_nested_expressions::GetUsage(std::forward<Args>(args)...);
  }

  auto BuildExpr(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    auto *expr = ctx.expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const QualType final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      auto type = decl_ref_expr->getType();
      if (type->isIntegerType() && ctx.usage_kind == Usage::Value) {
        auto is_signed = type->isSignedIntegerType();
        auto bit_width = data.Ctx.getTypeSize(type);
        if (auto error = VerifyTypeWidth(type)) {
          return error;
        }

        if (bit_width == GetPointerWidth(data.Ctx) && !is_signed) {
          goto decl_ref_expr_general_case;
        }

        if (!is_signed) {
          goto decl_ref_expr_general_case;
        }

        auto decl_ref_expr_source_text = GetSourceText(decl_ref_expr, data.Ctx);
        if (auto error = decl_ref_expr_source_text.takeError()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to get source text for DeclRefExpr: {1}",
                                      decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
        auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                                GetPointerWidth(data.Ctx), *decl_ref_expr_source_text);
        auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                        is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
        os << final_conv;
      } else {
      decl_ref_expr_general_case:
        if (auto error = PrintSourceText(os, decl_ref_expr, data.Ctx)) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to print source text for DeclRefExpr: {1}",
                                      decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
      }
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      data.need_int_helpers = true;

      auto type = integer_literal->getType();
      auto is_signed = type->isSignedIntegerType();
      auto bit_width = data.Ctx.getTypeSize(type);
      if (auto error = VerifyTypeWidth(type)) {
        return error;
      }

      if (bit_width == GetPointerWidth(data.Ctx) && !is_signed) {
        if (auto error = PrintSourceText(os, integer_literal, data.Ctx)) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to print source text for IntegerLiteral: {1}",
                                      integer_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
      } else if (!is_signed) {
        if (auto error = PrintSourceText(os, integer_literal, data.Ctx)) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to print source text for IntegerLiteral: {1}",
                                      integer_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
      } else {
        auto integer_literal_source_text = GetSourceText(integer_literal, data.Ctx);
        if (auto error = integer_literal_source_text.takeError()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to get source text for IntegerLiteral: {1}",
                                      integer_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
        auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                                GetPointerWidth(data.Ctx), *integer_literal_source_text);
        auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                        is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
        os << final_conv;
      }

    } else if (auto *_ = dyn_cast<FloatingLiteral>(expr)) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nFloating point literals are not allowed",
                                  expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      data.need_int_helpers = true;
      auto type = character_literal->getType();
      auto is_signed = type->isSignedIntegerType();
      auto bit_width = data.Ctx.getTypeSize(type);
      if (auto error = VerifyTypeWidth(type)) {
        return error;
      }

      if (!is_signed) {
        if (auto error = PrintSourceText(os, character_literal, data.Ctx)) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to print source text for CharacterLiteral: {1}",
                                      character_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
      } else {
        auto character_literal_source_text = GetSourceText(character_literal, data.Ctx);
        if (auto error = character_literal_source_text.takeError()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to get source text for CharacterLiteral: {1}",
                                      character_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
        auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                                GetPointerWidth(data.Ctx), *character_literal_source_text);
        auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                        is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
        os << final_conv;
      }
    } else if (auto *string_literal = dyn_cast<StringLiteral>(expr)) {
      if (auto error = PrintSourceText(os, string_literal, data.Ctx)) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to print source text for StringLiteral: {1}",
                                    string_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(error)))));
      }
    } else if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
      if (auto error = res.takeError()) {
        return error;
      }

      if (c_style_cast_expr->getCastKind() == CK_IntegralCast) {
        data.need_int_helpers = true;
        // although src_type might be different in the original code, it should just be a uint64_t at this point
        auto src_type = sub_expr->getType();
        if (auto error = VerifyTypeWidth(src_type)) {
          return error;
        }

        auto dest_type = c_style_cast_expr->getType();
        auto dest_is_signed = dest_type->isSignedIntegerType();
        auto dest_bit_width = data.Ctx.getTypeSize(dest_type);
        if (auto error = VerifyTypeWidth(dest_type)) {
          return error;
        }

        if (dest_bit_width == GetPointerWidth(data.Ctx)) {
          goto c_style_cast_general_case;
        } else {
          os << llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                              dest_is_signed ? 'i' : 'u', dest_bit_width, res->final_expr);
        }
      } else {
      c_style_cast_general_case:
        os << "(";
        c_style_cast_expr->getTypeAsWritten().print(os, data.Ctx.getPrintingPolicy());
        os << ")(";
        os << res->final_expr;
        os << ")";
      }

      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      if (ctx.usage_kind == Usage::Place) {
        return CreateRuntimeError(
            std::move(llvm::formatv("Binary operator cannot be used as a place expression, {1}",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
      if (auto error = rhs_res.takeError()) {
        return error;
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
      if (auto error = lhs_res.takeError()) {
        return error;
      }

      switch (binary_operator->getOpcode()) {
      case BO_Div:
        [[fallthrough]];
      case BO_Rem: {
        if (!lhs->getType()->isIntegerType() || !rhs->getType()->isIntegerType()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("Binary operator (DIV, REM) operands must be integer types, {1}",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }
        data.need_int_helpers = true;

        auto lhs_type = lhs->getType();
        auto lhs_is_signed = lhs_type->isSignedIntegerType();
        auto lhs_bit_width = data.Ctx.getTypeSize(lhs_type);
        if (auto error = VerifyTypeWidth(lhs_type)) {
          return error;
        }

        auto bin_op_str = BinOpTypeToStr(binary_operator->getOpcode());
        if (auto error = bin_op_str.takeError()) {
          return error;
        }
        if (lhs_is_signed) {
          os << llvm::formatv("__c2pnk_i{0}_{1}({2}, {3}, {4})", GetPointerWidth(data.Ctx), *bin_op_str,
                              lhs_res->final_expr, rhs_res->final_expr, lhs_bit_width);
        } else {
          os << llvm::formatv("__c2pnk_u{0}_{1}({2}, {3})", GetPointerWidth(data.Ctx), *bin_op_str, lhs_res->final_expr,
                              rhs_res->final_expr);
        }
        break;
      }

      case BO_Assign: {
        // clang should have inserted an implicit cast to turn the RHS into the same type as the LHS
        // this should then have been converted into an explicit cast
        // which would have been handled at this point already
        // so there's nothing to do...

        // we want to build with the LHS as a place expression, and the RHS as a value expression
        lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place, ctx.deref_force_extract, ctx.assigned_to));
        if (auto error = lhs_res.takeError()) {
          return error;
        }

        pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res->final_expr, rhs_res->final_expr).str());
        if (ctx.usage_kind == Usage::Value) {
          os << lhs_res->final_expr;
        }
        break;
      }

      case BO_Shr: {
        if (!lhs->getType()->isIntegerType() || !rhs->getType()->isIntegerType()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("Binary operator (SHR) operands must be integer types, {1}",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }
        data.need_int_helpers = true;

        auto lhs_type = lhs->getType();
        auto lhs_is_signed = lhs_type->isSignedIntegerType();
        // auto lhs_bit_width = data.Ctx.getTypeSize(lhs_type);
        if (auto error = VerifyTypeWidth(lhs_type)) {
          return error;
        }

        auto rhs_type = rhs->getType();
        if (auto error = VerifyTypeWidth(rhs_type)) {
          return error;
        }

        if (lhs_is_signed) {
          os << llvm::formatv("__c2pnk_i{0}_sar({1}, {2})", GetPointerWidth(data.Ctx), lhs_res->final_expr,
                              rhs_res->final_expr);
        } else {
          os << llvm::formatv("({0} >> {1})", lhs_res->final_expr, rhs_res->final_expr);
        }
        break;
      }
      case BO_LT:
        [[fallthrough]];
      case BO_GT:
        [[fallthrough]];
      case BO_LE:
        [[fallthrough]];
      case BO_GE: {
        if (!lhs->getType()->isIntegerType() || !rhs->getType()->isIntegerType()) {
          // possible for pointer types to be compared....
          goto binary_operator_general_case;
        }

        data.need_int_helpers = true;
        if (lhs->getType()->getCanonicalTypeUnqualified() != rhs->getType()->getCanonicalTypeUnqualified()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("Binary operator (LT, GT, LE, GE) operands must have the same type, {1}",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        auto type = lhs->getType();
        auto is_signed = type->isSignedIntegerType();
        // auto bit_width = data.Ctx.getTypeSize(type);
        if (auto error = VerifyTypeWidth(type)) {
          return error;
        }

        auto bin_op_str = BinOpTypeToStr(binary_operator->getOpcode());
        if (auto error = bin_op_str.takeError()) {
          return error;
        }
        if (is_signed) {
          os << llvm::formatv("__c2pnk_i{0}_{1}({2}, {3})", GetPointerWidth(data.Ctx), *bin_op_str, lhs_res->final_expr,
                              rhs_res->final_expr);
        } else {
          os << llvm::formatv("({0} {1} {2})", lhs_res->final_expr,
                              BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res->final_expr);
        }
        break;
      }

      case BO_Mul:
        [[fallthrough]];
      case BO_Add:
        [[fallthrough]];
      case BO_Sub:
        [[fallthrough]];
      case BO_Shl:
        [[fallthrough]];
      case BO_EQ:
        [[fallthrough]];
      case BO_NE:
        [[fallthrough]];
      case BO_And:
        [[fallthrough]];
      case BO_Xor:
        [[fallthrough]];
      case BO_Or: {
      binary_operator_general_case:
        // don't return a pre stmt no matter what since these can be arbitrarily nested
        os << llvm::formatv("({0} {1} {2})", lhs_res->final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res->final_expr);
        break;
      }

      default: {
        return CreateRuntimeError(std::move(llvm::formatv(
            "Unhandled binary operator: {0}, {1}", BinaryOperator::getOpcodeStr(binary_operator->getOpcode()),
            binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        break;
      }
      }

      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();

      switch (unary_operator->getOpcode()) {
      case UO_Deref: {
        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, true, ctx.assigned_to));
        if (auto error = res.takeError()) {
          return error;
        }
        os << "*" << res->final_expr;
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      case UO_AddrOf: {
        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place, ctx.deref_force_extract, ctx.assigned_to));
        if (auto error = res.takeError()) {
          return error;
        }
        os << llvm::formatv("{0}{1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res->final_expr);
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      case UO_Minus: {
        // this will not work correctly on smaller types that are located within a u64
        // we need to mask the u64 to zero the high bits after applying the unary minus

        auto type = sub_expr->getType();
        if (type->isIntegerType()) {
          data.need_int_helpers = true;
          auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
          if (auto error = res.takeError()) {
            return error;
          }

          auto is_signed = type->isSignedIntegerType();
          auto bit_width = data.Ctx.getTypeSize(type);
          if (auto error = VerifyTypeWidth(type)) {
            return error;
          }

          if (bit_width == GetPointerWidth(data.Ctx)) {
            os << llvm::formatv("-{0}", res->final_expr);
          } else {
            os << llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}(-{3})", GetPointerWidth(data.Ctx), is_signed ? 'i' : 'u',
                                bit_width, res->final_expr);
          }

        } else {
          goto unary_operator_general_case;
        }
        break;
      }

      case UO_Plus:
        [[fallthrough]];
      case UO_Not:
        [[fallthrough]];
      case UO_LNot:
        [[fallthrough]];
      case UO_Real:
        [[fallthrough]];
      case UO_Imag:
        [[fallthrough]];
      case UO_Extension: {
      unary_operator_general_case:
        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
        if (auto error = res.takeError()) {
          return error;
        }
        os << llvm::formatv("{0}{1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res->final_expr);
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      default: {
        return CreateRuntimeError(std::move(llvm::formatv(
            "Unhandled unary operator: {0}, {1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(),
            unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      }
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        auto res =
            BuildExpr(BuildExprCtx(arg->IgnoreParenImpCasts(), Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
        if (auto error = res.takeError()) {
          return error;
        }
        arg_built_exprs.push_back(*res);
      }
      // it's possible for getDirectCallee to return nullptr, but I don't know what to do in that case...
      // TODO throw error on any function pointers
      os << llvm::formatv("{0}({1})", call_expr->getDirectCallee()->getName().str(),
                          llvm::join(arg_built_exprs | std::views::transform([this](const BuiltExpr &e) -> std::string {
                                       // we need to do a check here because of functions that take format strings like
                                       // printf. the "implicit" casts cannot be fixed by the previous pass.
                                       if (e.final_expr_type->isIntegerType()) {
                                         // prevent sizeof exprs from returning __size_t which is impl defined...
                                         auto final_expr_type = e.final_expr_type.getAsString(data.Ctx.getLangOpts());
                                         if (final_expr_type == "__size_t") {
                                           final_expr_type = "size_t";
                                         }

                                         return llvm::formatv("({0}){1}", final_expr_type, e.final_expr).str();
                                       }
                                       return e.final_expr;
                                     }),
                                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      if (member_expr->isArrow()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("Arrow member access is not allowed, {1}",
                                    member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      auto res = BuildExpr(BuildExprCtx(member_expr->getBase()->IgnoreParenImpCasts(), Usage::Place,
                                        ctx.deref_force_extract, ctx.assigned_to));
      if (auto error = res.takeError()) {
        return error;
      }
      auto final_expr = llvm::formatv("({0}).{1}", res->final_expr, member_expr->getMemberNameInfo().getAsString());

      auto type = member_expr->getType();
      if (type->isIntegerType() && ctx.usage_kind == Usage::Value) {
        data.need_int_helpers = true;
        auto is_signed = type->isSignedIntegerType();
        auto bit_width = data.Ctx.getTypeSize(type);
        if (auto error = VerifyTypeWidth(type)) {
          return error;
        }

        if (!is_signed) {
          os << final_expr;
        } else {
          os << llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                              GetPointerWidth(data.Ctx), final_expr);
        }
      } else {
        os << final_expr;
      }

      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else {
      if (auto error = PrintSourceText(os, expr, data.Ctx)) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nFailed to print source text for expression: {1}",
            expr->getExprLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(error)))));
      }
    }

    os.flush();
    return BuiltExpr(pre_stmts, final_expr, final_expr_type);
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    auto *prev_func_decl = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto res = RecursiveASTVisitor::TraverseFunctionDecl(func_decl);
    data.current_function_decl = prev_func_decl;
    return res;
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (declStmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(declStmt->getBeginLoc())) ||
        data.current_function_decl == nullptr) {
      return true;
    }

    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);

    for (auto *decl : declStmt->decls()) {
      if (auto *var_decl = dyn_cast<VarDecl>(decl)) {
        auto *init_expr = var_decl->getInit();
        if (init_expr != nullptr) {
          // normally we would put Usage::Value here but since every Usage::Place is also a Usage::Value, we can just
          // use Usage::Place to avoid an extra copy of the expression
          init_expr = init_expr->IgnoreParenImpCasts();
          auto res = BuildExpr(BuildExprCtx(
              init_expr, Usage::Value, false,
              std::make_optional(std::make_pair(var_decl->getNameAsString(), var_decl->getType().getCanonicalType()))));
          if (auto error = res.takeError()) {
            data.error = std::move(error);
            return false;
          }

          // if the final expression is empty, the decl comes first.
          // this is because the init expr stuff needs to come AFTER the decl...
          if (res->final_expr.empty()) {
            os << PrintType(var_decl->getType(), var_decl->getName()) << ";";
          }
          os << llvm::join(res->pre_stmts | std::views::reverse, "\n");

          if (!res->final_expr.empty()) {
            // assign whatever final value there is...
            os << llvm::formatv("{0} = {1};", PrintType(var_decl->getType(), var_decl->getName()), res->final_expr);
          }
        } else {
          os << PrintType(var_decl->getType(), var_decl->getName()) << ";";
        }
      }
    }

    os.flush();
    if (!replacement_text.empty()) {
      data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                     CharSourceRange::getTokenRange(declStmt->getSourceRange()), replacement_text,
                                     data.Ctx.getLangOpts());
      return true;
    }

    return RecursiveASTVisitor::TraverseDeclStmt(declStmt);
  }

  auto TraverseStmt(Stmt *stmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(stmt->getBeginLoc())) ||
        data.current_function_decl == nullptr) {
      return true;
    }

    if (auto *expr = dyn_cast<Expr>(stmt)) {
      expr = expr->IgnoreParenImpCasts();

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      auto res = BuildExpr(BuildExprCtx(expr, Usage::Effect, false, std::nullopt));
      if (auto error = res.takeError()) {
        data.error = std::move(error);
        return false;
      }

      os << llvm::join(res->pre_stmts | std::views::reverse, "\n");

      if (!res->final_expr.empty()) {
        os << llvm::formatv("{0}", res->final_expr);
      }
      os.flush();

      if (!replacement_text.empty()) {
        CharSourceRange range = CharSourceRange::getTokenRange(stmt->getSourceRange());
        if (res->final_expr.empty()) {
          auto start = stmt->getBeginLoc();
          auto end = stmt->getEndLoc();
          auto end_inc_semicolon =
              Lexer::findLocationAfterToken(end, tok::semi, data.Ctx.getSourceManager(), data.Ctx.getLangOpts(), false);
          range = CharSourceRange::getCharRange(start, end_inc_semicolon);
        } else {
          range = CharSourceRange::getTokenRange(stmt->getSourceRange());
        }

        data.replacements.emplace_back(data.Ctx.getSourceManager(), range, replacement_text, data.Ctx.getLangOpts());
      }
      return true;
    }

    return RecursiveASTVisitor::TraverseStmt(stmt);
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = ps_ctx, .replacements = replacements, .need_int_helpers = false};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (auto error = std::move(data.error)) {
    ps_ctx.error = std::move(error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  if (data.need_int_helpers) {
    replacements.emplace_back(Ctx.getSourceManager(),
                              Ctx.getSourceManager().getLocForStartOfFile(Ctx.getSourceManager().getMainFileID()), 0,
                              GetPointerWidth(Ctx) == 32 ? signed_i32_helpers : signed_i64_helpers);
  }

  for (const auto &r : replacements) {
    if (r.getFilePath().empty()) {
      // if there's no file path then it means the replacement is some fucked macro expansion or whatever
      // anyway, I don't know if there is any case where we do want to apply the replacement (I'm not even sure how
      // these got generated in the first place :skull:).
      continue;
    }

    if (auto err = ps_ctx.replacements.add(r)) {
      ps_ctx.error = CreateRuntimeError(llvm::formatv("Add replacement conflict: {0}", err));
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_integer_conversion
