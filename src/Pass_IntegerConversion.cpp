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
                  changeTo(node("cast"), cat("(", TypeName("cast"), ")(", node("cast"), ")")));
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
const char *signed_i64_helpers = R"(#include <assert.h>
#include <stdint.h>
/* c2pancake generated code start: helpers for i64 signed ops */

uint64_t __c2pnk_u8_to_u64(uint8_t x) {
    // noop
    return (uint64_t)x;
}

uint64_t __c2pnk_u16_to_u64(uint16_t x) {
    // noop
    return (uint64_t)x;
}

uint64_t __c2pnk_u32_to_u64(uint32_t x) {
    // noop
    return (uint64_t)x;
}

uint64_t __c2pnk_u64_to_u64(uint64_t x) {
    // noop
    return x;
}

uint64_t __c2pnk_i8_to_u64(int8_t x) {
    uint64_t y = (uint8_t)x;
    return (y ^ (uint64_t)0x80) - (uint64_t)0x80;
}

uint64_t __c2pnk_i16_to_u64(int16_t x) {
    uint64_t y = (uint16_t)x;
    return (y ^ (uint64_t)0x8000) - (uint64_t)0x8000;
}

uint64_t __c2pnk_i32_to_u64(int32_t x) {
    uint64_t y = (uint32_t)x;
    return (y ^ (uint64_t)0x80000000) - (uint64_t)0x80000000;
}

uint64_t __c2pnk_i64_to_u64(int64_t x) {
    // noop
    return (uint64_t)x;
}

uint64_t __c2pnk_i64_lt(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a < b;
    }
}

uint64_t __c2pnk_i64_lte(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a <= b;
    }
}

uint64_t __c2pnk_i64_gt(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a > b;
    }
}

uint64_t __c2pnk_i64_gte(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63;
    uint64_t sb = b >> 63;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a >= b;
    }
}

/* arithmetic right shift */
uint64_t __c2pnk_i64_sar(uint64_t a, uint64_t n) {
    assert(n > 0);
    assert(n < 64);

    uint64_t sign = a >> 63;
    uint64_t mask = 0 - sign;
    uint64_t fill = mask << (64 - n);

    return (a >> n) | fill;
}

uint64_t __c2pnk_trunc_u64_to_u64(uint64_t x) {
    // noop
    return x;
}

uint64_t __c2pnk_trunc_u64_to_u32(uint64_t x) {
    return x & (uint64_t)0xffffffff;
}

uint64_t __c2pnk_trunc_u64_to_u16(uint64_t x) {
    return x & (uint64_t)0xffff;
}

uint64_t __c2pnk_trunc_u64_to_u8(uint64_t x) {
    return x & (uint64_t)0xff;
}

uint64_t __c2pnk_trunc_u64_to_i64(uint64_t x) {
    // noop
    return x;
}

uint64_t __c2pnk_trunc_u64_to_i32(uint64_t x) {
  uint64_t y = x & (uint64_t)0xffffffff;
  return (y ^ (uint64_t)0x80000000) - (uint64_t)0x80000000;
}

uint64_t __c2pnk_trunc_u64_to_i16(uint64_t x) {
  uint64_t y = x & (uint64_t)0xffff;
  return (y ^ (uint64_t)0x8000) - (uint64_t)0x8000;
}

uint64_t __c2pnk_trunc_u64_to_i8(uint64_t x) {
  uint64_t y = x & (uint64_t)0xff;
  return (y ^ (uint64_t)0x80) - (uint64_t)0x80;
}
/* c2pancake generated code end: helpers for i64 signed ops */

)";

const char *signed_i32_helpers = R"(#include <assert.h>
#include <stdint.h>
/* c2pancake generated code start: helpers for i32 signed ops */
uint32_t __c2pnk_i8_to_u32(int8_t x) {
    uint32_t y = (uint8_t)x;
    return (y ^ (uint32_t)0x80) - (uint32_t)0x80;
}

uint32_t __c2pnk_i16_to_u32(int16_t x) {
    uint32_t y = (uint16_t)x;
    return (y ^ (uint32_t)0x8000) - (uint32_t)0x8000;
}

uint32_t __c2pnk_i32_to_u32(int32_t x) {
    // noop
    return (uint32_t)x;
}

uint32_t __c2pnk_u8_to_u32(uint8_t x) {
    // noop
    return (uint32_t)x;
}

uint32_t __c2pnk_u16_to_u32(uint16_t x) {
    // noop
    return (uint32_t)x;
}

uint32_t __c2pnk_u32_to_u32(uint32_t x) {
    // noop
    return x;
}

uint32_t __c2pnk_i32_lt(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a < b;
    }
}

uint32_t __c2pnk_i32_lte(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {
        return sa > sb;
    } else {
        return a <= b;
    }
}

uint32_t __c2pnk_i32_gt(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a > b;
    }
}

uint32_t __c2pnk_i32_gte(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31;
    uint32_t sb = b >> 31;

    if (sa != sb) {
        return sa < sb;
    } else {
        return a >= b;
    }
}

uint32_t __c2pnk_i32_sar(uint32_t a, uint32_t n) {
    assert(n > 0);
    assert(n < 32);

    uint32_t sign = a >> 31;
    uint32_t mask = 0 - sign;
    uint32_t fill = mask << (32 - n);

    return (a >> n) | fill;
}

uint32_t __c2pnk_trunc_u32_to_u32(uint32_t x) {
    // noop
    return x;
}

uint32_t __c2pnk_trunc_u32_to_u16(uint32_t x) {
    return x & (uint32_t)0xffff;
}

uint32_t __c2pnk_trunc_u32_to_u8(uint32_t x) {
    return x & (uint32_t)0xff;
}

uint32_t __c2pnk_trunc_u32_to_i32(uint32_t x) {
    // noop
    return (uint32_t)x;
}

uint32_t __c2pnk_trunc_u32_to_i16(uint32_t x) {
    uint32_t y = x & (uint32_t)0xffff;
    return (y ^ (uint32_t)0x8000) - (uint32_t)0x8000;
}

uint32_t __c2pnk_trunc_u32_to_i8(uint32_t x) {
    uint32_t y = x & (uint32_t)0xff;
    return (y ^ (uint32_t)0x80) - (uint32_t)0x80;
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

  bool shouldTraversePostOrder() const { return true; }

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

  auto VerifyTypeWidth(const QualType type) const -> void {
    auto width = data.Ctx.getTypeSize(type);
    if (GetPointerWidth(data.Ctx) == 32) {
      if (width == 32 || width == 16 || width == 8) {
        // ok
      } else {
        llvm::errs() << llvm::formatv("{0} Unsupported type width for 32-bit target: {1}\n", LogBegin(data.pa_ctx),
                                      width);
        llvm_unreachable("Unsupported type width for 32-bit target");
      }
    } else if (GetPointerWidth(data.Ctx) == 64) {
      if (width == 64 || width == 32 || width == 16 || width == 8) {
        // ok
      } else {
        llvm::errs() << llvm::formatv("{0} Unsupported type width for 64-bit target: {1}\n", LogBegin(data.pa_ctx),
                                      width);
        llvm_unreachable("Unsupported type width for 64-bit target");
      }
    } else {
      llvm_unreachable("Unsupported pointer width");
    }
  }

  static auto BinOpTypeToStr(const BinaryOperatorKind op) -> std::string {
    switch (op) {
    case BO_LT:
      return "lt";
    case BO_GT:
      return "gt";
    case BO_LE:
      return "lte";
    case BO_GE:
      return "gte";
    default:
      llvm_unreachable("Unsupported binary operator");
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

  auto BuildExpr(const BuildExprCtx &ctx) -> BuiltExpr {
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
        VerifyTypeWidth(type);

        if (bit_width == GetPointerWidth(data.Ctx) && !is_signed) {
          goto decl_ref_expr_general_case;
        }

        auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                                GetPointerWidth(data.Ctx), GetSourceText(decl_ref_expr, data.Ctx));
        auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                        is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
        os << final_conv;
      } else {
      decl_ref_expr_general_case:
        PrintSourceText(os, decl_ref_expr, data.Ctx);
      }
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      data.need_int_helpers = true;

      auto type = integer_literal->getType();
      auto is_signed = type->isSignedIntegerType();
      auto bit_width = data.Ctx.getTypeSize(type);
      VerifyTypeWidth(type);

      if (bit_width == GetPointerWidth(data.Ctx) && !is_signed) {
        PrintSourceText(os, integer_literal, data.Ctx);
      } else {
        auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                                GetPointerWidth(data.Ctx), GetSourceText(integer_literal, data.Ctx));
        auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                        is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
        os << final_conv;
      }

    } else if (auto *_ = dyn_cast<FloatingLiteral>(expr)) {
      llvm::errs() << llvm::formatv("{0} Floating point literals are not allowed, {1}\n", LogBegin(data.pa_ctx),
                                    expr->getExprLoc().printToString(data.Ctx.getSourceManager()));
      llvm_unreachable("Floating point literals are not allowed!");
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      data.need_int_helpers = true;
      auto type = character_literal->getType();
      auto is_signed = type->isSignedIntegerType();
      auto bit_width = data.Ctx.getTypeSize(type);
      VerifyTypeWidth(type);

      auto signed_to_unsigned = llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width,
                                              GetPointerWidth(data.Ctx), GetSourceText(character_literal, data.Ctx));
      auto final_conv = llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx),
                                      is_signed ? 'i' : 'u', bit_width, signed_to_unsigned);
      os << final_conv;
    } else if (auto *string_literal = dyn_cast<StringLiteral>(expr)) {
      PrintSourceText(os, string_literal, data.Ctx);
    } else if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));

      if (c_style_cast_expr->getCastKind() == CK_IntegralCast) {
        data.need_int_helpers = true;
        // although src_type might be different in the original code, it should just be a uint64_t at this point
        auto src_type = sub_expr->getType();
        VerifyTypeWidth(src_type);

        auto dest_type = c_style_cast_expr->getType();
        auto dest_is_signed = dest_type->isSignedIntegerType();
        auto dest_bit_width = data.Ctx.getTypeSize(dest_type);
        VerifyTypeWidth(dest_type);

        os << llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}({3})", GetPointerWidth(data.Ctx), dest_is_signed ? 'i' : 'u',
                            dest_bit_width, res.final_expr);
      } else {
        os << "(";
        c_style_cast_expr->getTypeAsWritten().print(os, data.Ctx.getPrintingPolicy());
        os << ")(";
        os << res.final_expr;
        os << ")";
      }

      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      if (ctx.usage_kind == Usage::Place) {
        llvm::errs() << llvm::formatv("{0} Binary operator cannot be used as a place expression, {1}\n",
                                      LogBegin(data.pa_ctx),
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()));
        llvm_unreachable("Binary operator cannot be used as a place expression");
      }

      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place, ctx.deref_force_extract, ctx.assigned_to));

      switch (binary_operator->getOpcode()) {
      case BO_Div:
        [[fallthrough]];
      case BO_Rem: {
        llvm_unreachable("Division and remainder operators are not allowed!");
      }

      case BO_Assign: {
        // clang should have inserted an implicit cast to turn the RHS into the same type as the LHS
        // this should then have been converted into an explicit cast
        // which would have been handled at this point already
        // so there's nothing to do...
        pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res.final_expr, rhs_res.final_expr).str());
        if (ctx.usage_kind == Usage::Value) {
          os << lhs_res.final_expr;
        }
        break;
      }

      case BO_Shr: {
        assert(lhs->getType()->isIntegerType() && rhs->getType()->isIntegerType() &&
               "Binary operator (SHR) operands must be integer types");
        data.need_int_helpers = true;

        auto lhs_type = lhs->getType();
        auto lhs_is_signed = lhs_type->isSignedIntegerType();
        // auto lhs_bit_width = data.Ctx.getTypeSize(lhs_type);
        VerifyTypeWidth(lhs_type);

        auto rhs_type = rhs->getType();
        VerifyTypeWidth(rhs_type);

        if (lhs_is_signed) {
          os << llvm::formatv("__c2pnk_i{0}_sar({1}, {2})", GetPointerWidth(data.Ctx), lhs_res.final_expr,
                              rhs_res.final_expr);
        } else {
          os << llvm::formatv("({0} >> {1})", lhs_res.final_expr, rhs_res.final_expr);
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

        assert(lhs->getType()->getCanonicalTypeUnqualified() == rhs->getType()->getCanonicalTypeUnqualified() &&
               "Binary operator (LT, GT, LE, GE) operands must have the same type");

        auto type = lhs->getType();
        auto is_signed = type->isSignedIntegerType();
        // auto bit_width = data.Ctx.getTypeSize(type);
        VerifyTypeWidth(type);

        if (is_signed) {
          os << llvm::formatv("__c2pnk_i{0}_{1}({2}, {3})", GetPointerWidth(data.Ctx),
                              BinOpTypeToStr(binary_operator->getOpcode()), lhs_res.final_expr, rhs_res.final_expr);
        } else {
          os << llvm::formatv("({0} {1} {2})", lhs_res.final_expr,
                              BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res.final_expr);
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
        os << llvm::formatv("({0} {1} {2})", lhs_res.final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res.final_expr);
        break;
      }

      default: {
        llvm::errs() << llvm::formatv("{0} Unhandled binary operator: {1}, {2}\n", LogBegin(data.pa_ctx),
                                      BinaryOperator::getOpcodeStr(binary_operator->getOpcode()),
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()));
        llvm_unreachable("Unhandled binary operator");
        break;
      }
      }

      pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      // auto res = BuildExpr(sub_expr, depth + 1);

      switch (unary_operator->getOpcode()) {
      case UO_Deref: {
        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, true, ctx.assigned_to));
        os << "*" << res.final_expr;
        pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
        break;
      }

      case UO_AddrOf: {
        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place, ctx.deref_force_extract, ctx.assigned_to));
        os << llvm::formatv("{0}{1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res.final_expr);
        pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
        break;
      }

      case UO_Minus: {
        // this will not work correctly on smaller types that are located within a u64
        // we need to mask the u64 to zero the high bits after applying the unary minus

        auto type = sub_expr->getType();
        if (type->isIntegerType()) {
          data.need_int_helpers = true;
          auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to));

          auto is_signed = type->isSignedIntegerType();
          auto bit_width = data.Ctx.getTypeSize(type);
          VerifyTypeWidth(type);

          os << llvm::formatv("__c2pnk_trunc_u{0}_to_{1}{2}(-{3})", GetPointerWidth(data.Ctx), is_signed ? 'i' : 'u',
                              bit_width, res.final_expr);
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
        os << llvm::formatv("{0}{1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res.final_expr);
        pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
        break;
      }

      default: {
        llvm_unreachable("Unhandled unary operator");
        break;
      }
      }
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        arg_built_exprs.push_back(BuildExpr(
            BuildExprCtx(arg->IgnoreParenImpCasts(), Usage::Value, ctx.deref_force_extract, ctx.assigned_to)));
      }
      // it's possible for getDirectCallee to return nullptr, but I don't know what to do in that case...
      // TODO throw error on any function pointers
      os << llvm::formatv(
          "{0}({1})", call_expr->getDirectCallee()->getName().str(),
          llvm::join(arg_built_exprs | std::views::transform([this](const BuiltExpr &e) -> std::string {
                       // we need to do a check here because of functions that take format strings like
                       // printf. the "implicit" casts cannot be fixed by the previous pass.
                       if (e.final_expr_type->isIntegerType()) {

                         return llvm::formatv("({0}){1}", e.final_expr_type.getAsString(data.Ctx.getPrintingPolicy()),
                                              e.final_expr)
                             .str();
                       }
                       return e.final_expr;
                     }),
                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      assert(!member_expr->isArrow() && "Only dot member access is allowed here");
      auto res = BuildExpr(BuildExprCtx(member_expr->getBase()->IgnoreParenImpCasts(), Usage::Place,
                                        ctx.deref_force_extract, ctx.assigned_to));
      auto final_expr = llvm::formatv("({0}).{1}", res.final_expr, member_expr->getMemberNameInfo().getAsString());

      auto type = member_expr->getType();
      if (type->isIntegerType() && ctx.usage_kind == Usage::Value) {
        data.need_int_helpers = true;
        auto is_signed = type->isSignedIntegerType();
        auto bit_width = data.Ctx.getTypeSize(type);
        VerifyTypeWidth(type);

        os << llvm::formatv("__c2pnk_{0}{1}_to_u{2}({3})", is_signed ? 'i' : 'u', bit_width, GetPointerWidth(data.Ctx),
                            final_expr);
      } else {
        os << final_expr;
      }

      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else {
      PrintSourceText(os, expr, data.Ctx);
    }

  build_expr_end:
    os.flush();
    return BuiltExpr(pre_stmts, final_expr, final_expr_type);
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    auto &sm = data.Ctx.getSourceManager();
    if (declStmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(declStmt->getBeginLoc()))) {
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

          // if the final expression is empty, the decl comes first.
          // this is because the init expr stuff needs to come AFTER the decl...
          if (res.final_expr.empty()) {
            os << PrintType(var_decl->getType(), var_decl->getName()) << ";";
          }
          os << llvm::join(res.pre_stmts | std::views::reverse, "\n");

          if (!res.final_expr.empty()) {
            // assign whatever final value there is...
            os << llvm::formatv("{0} = {1};", PrintType(var_decl->getType(), var_decl->getName()), res.final_expr);
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
    auto &sm = data.Ctx.getSourceManager();
    if (stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(stmt->getBeginLoc()))) {
      return true;
    }

    if (auto *expr = dyn_cast<Expr>(stmt)) {
      expr = expr->IgnoreParenImpCasts();

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      auto res = BuildExpr(BuildExprCtx(expr, Usage::Effect, false, std::nullopt));
      os << llvm::join(res.pre_stmts | std::views::reverse, "\n");

      if (!res.final_expr.empty()) {
        os << llvm::formatv("{0}", res.final_expr);
      }
      os.flush();

      if (!replacement_text.empty()) {
        CharSourceRange range = CharSourceRange::getTokenRange(stmt->getSourceRange());
        if (res.final_expr.empty()) {
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
  WorkerData data{.Ctx = Ctx, .pa_ctx = pa_ctx, .replacements = replacements, .need_int_helpers = false};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.need_int_helpers) {
    replacements.emplace_back(Ctx.getSourceManager(),
                              Ctx.getSourceManager().getLocForStartOfFile(Ctx.getSourceManager().getMainFileID()), 0,
                              GetPointerWidth(Ctx) == 32 ? signed_i32_helpers : signed_i64_helpers);
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
  assert(!add_error_occurred && "Singleshot pass");
}
} // namespace pancake::pass_integer_conversion
