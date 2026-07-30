#include "Pass_LowerBitfieldOps.h"
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
#include <clang/Basic/TokenKinds.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
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
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::transformer;
using namespace clang::ast_matchers;

namespace pancake::pass_lower_bitfield_ops {
namespace {
// This helper code facilitates non-volatile bitfield operations.
const char *non_volatile_bitfield_helpers =
    R"(/* c2pancake generated code start: helpers for non-volatile bitfield operations */
#include <stdint.h>
static inline uint{0}_t __c2pnk_get_bit_u{0}(uint{0}_t value, uint{0}_t bit) {{ return ((value >> bit) & 1UL) != 0UL; }

static inline uint{0}_t __c2pnk_set_bit(uint8_t *byte, uint{0}_t bit) {{
  uint{0}_t val = (uint{0}_t)*byte;
  // truncation
  *byte = (uint8_t)(val | (uint{0}_t)(1UL << bit));
  return 0UL;
}

static inline uint{0}_t __c2pnk_clear_bit(uint8_t *byte, uint{0}_t bit) {{
  uint{0}_t val = (uint{0}_t)*byte;
  // truncation
  *byte = (uint8_t)(val & ~(1UL << bit));
  return 0UL;
}

/* [lhs_bit, rhs_bit) */
static uint{0}_t __c2pnk_set_bitfield_u{0}(uint{0}_t value, uint8_t *field, uint{0}_t lhs_bit, uint{0}_t rhs_bit) {{
  uint{0}_t width = rhs_bit - lhs_bit;

  uint{0}_t i = 0UL;
  while (i < width) {{
    uint{0}_t bit_index = lhs_bit + i;
    uint8_t *byte = &field[bit_index >> 3UL]; // / 8

    uint{0}_t cond = __c2pnk_get_bit_u{0}(value, i);
    uint{0}_t index = bit_index & 7UL; // % 8
    if (cond) {{
      (void)__c2pnk_set_bit(byte, index);
    } else {{
      (void)__c2pnk_clear_bit(byte, index);
    }

    i = i + 1UL;
  }

  return 0UL;
}

/* [lhs_bit, rhs_bit) */
static uint{0}_t __c2pnk_get_bitfield_u{0}(const uint8_t *field, uint{0}_t lhs_bit, uint{0}_t rhs_bit) {{
  uint{0}_t value = 0UL;
  uint{0}_t width = rhs_bit - lhs_bit;

  uint{0}_t i = 0UL;
  while (i < width) {{
    uint{0}_t bit_index = lhs_bit + i;

    uint{0}_t byte = (uint{0}_t)field[bit_index >> 3UL]; // / 8
    uint{0}_t index = bit_index & 7UL;                  // % 8
    uint{0}_t mask = (1UL << index);

    if (byte & mask) {{
      value = value | (1UL << i);
    }

    i = i + 1UL;
  }

  return value;
}

/* [lhs_bit, rhs_bit) */
static int{0}_t __c2pnk_get_bitfield_i{0}(const uint8_t *field, uint{0}_t lhs_bit, uint{0}_t rhs_bit) {{
  uint{0}_t value = __c2pnk_get_bitfield_u{0}(field, lhs_bit, rhs_bit);

  uint{0}_t width = rhs_bit - lhs_bit;

  /* manual sign-extend if the extracted field is narrower than {0} bits */
  if (width < {0}UL) {{
    uint{0}_t sign = 1UL << (width - 1UL);
    return (int{0}_t)((value ^ sign) - sign);
  }

  return (int{0}_t)value;
}

/* [lhs_bit, rhs_bit) */
static inline uint{0}_t __c2pnk_set_bitfield_i{0}(int{0}_t value, uint8_t *field, uint{0}_t lhs_bit, uint{0}_t rhs_bit) {{
  (void)__c2pnk_set_bitfield_u{0}((uint{0}_t)value, field, lhs_bit, rhs_bit);
  return 0UL;
}
/* c2pancake generated code end: helpers for non-volatile bitfield operations */

)";

struct WorkerData {
  ASTContext &Ctx;
  CodeGen::CodeGenModule &code_gen_module;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
  bool need_non_volatile_bitfield_helpers = false;
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

  static auto GetIntTypeName(unsigned bit_width, bool is_signed) -> std::string {
    const char *prefix = is_signed ? "int" : "uint";
    return llvm::formatv("{0}{1}_t", prefix, bit_width).str();
  }

  static auto FormatAPIntHex(const llvm::APInt &ap_int) -> std::string {
    llvm::SmallString<32> small_str;
    ap_int.toString(small_str, 16, false, true);
    return small_str.str().str();
  }

  static auto IsAAPCS(const TargetInfo &targetInfo) -> bool { return targetInfo.getABI().starts_with("aapcs"); }

  auto IsRead(const MemberExpr *member_expr) -> bool {
    const Stmt *stmt = member_expr;
    while (true) {
      auto parents = data.Ctx.getParentMapContext().getParents(*stmt);
      if (parents.empty())
        return false;

      // already did the check above...
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const auto *implicit_cast = parents[0].get<ImplicitCastExpr>();
      if (implicit_cast == nullptr)
        return false;

      switch (implicit_cast->getCastKind()) {
      case CK_LValueToRValue: {
        return true;
      }

      /*
      A CK_NoOp cast (qualification adjustment e.g., adding/dropping const/volatile on an lvalue, or a no-op cast to
      an equivalent type) can sit between the MemberExpr and the LValueToRValue cast.

      However, when a loaded value undergoes further conversion (integer promotion, usual arithmetic conversion,
      pointer decay of the result, etc.), those additional ImplicitCastExpr nodes wrap around the LValueToRValue cast
      so it's not a problem in this case.
      */
      case CK_NoOp: {
        stmt = implicit_cast;
        continue; // keep climbing
      }

      default: {
        return false; // ArrayToPointerDecay, etc.
      }
      }
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

  // Equivalent to
  // https://github.com/llvm/llvm-project/blob/2078da43e25a4623cab2d0d60decddf709aaea28/clang/lib/CodeGen/CGExpr.cpp#L2334
  // We only use this function for VOLATILE bitfields since the loads/stores generated should be always 8,16,32,64
  // If this ever changes, then we're fucked because clang IR can generate arbitrary width loads/stores
  auto BuildLoadOfBitFieldLValue(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    const MemberExpr *member_expr = cast<MemberExpr>(ctx.expr);
    // technically can't take address of a bitfield but Place also isn't a pointer...
    if (ctx.usage_kind != Usage::Value && ctx.usage_kind != Usage::Place) {
      return CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nUsage kind must be Value or Place, got {0}",
          member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), std::to_underlying(ctx.usage_kind))));
    }
    const FieldDecl *fd = cast<FieldDecl>(member_expr->getMemberDecl());
    if (!fd->isBitField()) {
      return CreateRuntimeError(std::move(
          llvm::formatv("\n    at {0}\nField {1} is not a bitfield",
                        member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), fd->getNameAsString())));
    }

    const CodeGen::CGBitFieldInfo &info =
        data.code_gen_module.getTypes().getCGRecordLayout(fd->getParent()).getBitFieldInfo(fd);

    QualType const ft = member_expr->getType();

    // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
    auto base_built_expr = BuildExpr(BuildExprCtx(member_expr->getBase(), Usage::Place, ctx.deref_force_extract,
                                                  ctx.assigned_to, ctx.string_literal_usage_kind));
    if (auto error = base_built_expr.takeError()) {
      return error;
    }
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string base_addr =
        member_expr->isArrow() ? base_built_expr->final_expr : llvm::formatv("(&{0})", base_built_expr->final_expr);

    const auto is_volatile = ft.isVolatileQualified();
    const auto use_volatile =
        is_volatile && info.VolatileStorageSize != 0 && IsAAPCS(data.code_gen_module.getTypes().getTarget());

    const auto offset = use_volatile ? info.VolatileOffset : info.Offset;
    const auto storage_size = use_volatile ? info.VolatileStorageSize : info.StorageSize;
    if (storage_size > GetPointerWidth(data.Ctx)) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nBitfield storage size {1} exceeds pointer width {2}",
                                  member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), storage_size,
                                  GetPointerWidth(data.Ctx))));
    }
    const auto storage_offset = use_volatile ? info.VolatileStorageOffset : info.StorageOffset;

    // Compute the storage-unit pointer, equivalent to LV.getBitFieldAddress()
    std::string storage_type_name = GetIntTypeName(storage_size, false);
    auto storage_ptr =
        llvm::formatv("(({0} *)((uint8_t *){1} + {2}LU))", storage_type_name, base_addr, storage_offset.getQuantity());

    std::string load_temp = GetTempVarName("bf_load");
    pre_stmts.push_back(llvm::formatv("{0} {1} {2} = *{3};", is_volatile ? "volatile " : "", storage_type_name,
                                      load_temp, storage_ptr));
    std::string current_temp = load_temp;

    if (info.IsSigned) {
      if (static_cast<unsigned>(offset + info.Size) > storage_size) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nBitfield offset {1} + size {2} exceeds storage size {3}",
            member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), offset, info.Size, storage_size)));
      }
      std::string signed_type_name = GetIntTypeName(storage_size, true);

      // Extract the field: (current_temp >> offset) & low_bits_mask(Size)
      // (equivalent to what the shl/ashr pair achieved implicitly, but as a plain unsigned extraction)
      std::string extracted_temp = current_temp;
      if (offset != 0U) {
        std::string lshr_temp = GetTempVarName("bf_lshr");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} >> {3}U;", storage_type_name, lshr_temp, extracted_temp, offset));
        extracted_temp = lshr_temp;
      }
      if (static_cast<unsigned>(offset) + info.Size < storage_size) {
        llvm::APInt const low_mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string mask_temp = GetTempVarName("bf_mask");
        pre_stmts.push_back(llvm::formatv("{0} {1} = {2} & {3}U;", storage_type_name, mask_temp, extracted_temp,
                                          FormatAPIntHex(low_mask)));
        extracted_temp = mask_temp;
      }
      current_temp = extracted_temp;

      // Sign-extend: sign = 1UL << (Size - 1); (T)((value ^ sign) - sign)
      llvm::APInt const sign_bit_mask = llvm::APInt::getOneBitSet(storage_size, info.Size - 1);
      std::string sign_temp = GetTempVarName("bf_sign");
      pre_stmts.push_back(
          llvm::formatv("{0} {1} = {2}U;", storage_type_name, sign_temp, FormatAPIntHex(sign_bit_mask)));

      std::string signed_temp = GetTempVarName("bf_signed");
      pre_stmts.push_back(
          llvm::formatv("{0} {1} = ({0})(({2} ^ {3}) - {3});", signed_type_name, signed_temp, current_temp, sign_temp));
      current_temp = signed_temp;
    } else {
      // Val = Builder.CreateLShr(Val, Offset, "bf.lshr");
      if (offset != 0U) {
        std::string lshr_temp = GetTempVarName("bf_lshr");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} >> {3}U;", storage_type_name, lshr_temp, current_temp, offset));
        current_temp = lshr_temp;
      }
      // Val = Builder.CreateAnd(Val, getLowBitsSet(StorageSize, Size), "bf.clear");
      if (static_cast<unsigned>(offset) + info.Size < storage_size) {
        llvm::APInt const mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string clear_temp = GetTempVarName("bf_clear");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} & {3}U;", storage_type_name, clear_temp, current_temp, FormatAPIntHex(mask)));
        current_temp = clear_temp;
      }
    }

    // not needed
    // Val = Builder.CreateIntCast(Val, ResLTy, IsSigned, "bf.cast");
    // std::string cast_temp = GetTempVarName("bf_cast");
    // pre_stmts.push_back(llvm::formatv("{0} {1} = ({0}){2};", ft.getAsString(), cast_temp, current_temp));

    auto final_pre_stmts = pre_stmts | std::views::reverse | std::ranges::to<llvm::SmallVector<std::string, 4>>();
    final_pre_stmts.insert(final_pre_stmts.end(), base_built_expr->pre_stmts.begin(), base_built_expr->pre_stmts.end());
    return BuiltExpr(std::move(final_pre_stmts), current_temp, ft);
  }

  // https://github.com/llvm/llvm-project/blob/2078da43e25a4623cab2d0d60decddf709aaea28/clang/lib/CodeGen/CGExpr.cpp#L2607
  auto BuildStoreThroughBitFieldLValue(const BuildExprCtx &ctx, Expr *src_expr) -> Expected<BuiltExpr> {
    const MemberExpr *member_expr = cast<MemberExpr>(ctx.expr);
    if (ctx.usage_kind != Usage::Effect) {
      return CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nUsage kind must be Effect, got {1}",
          ctx.expr->getExprLoc().printToString(data.Ctx.getSourceManager()), std::to_underlying(ctx.usage_kind))));
    }
    const FieldDecl *fd = cast<FieldDecl>(member_expr->getMemberDecl());
    if (!fd->isBitField()) {
      return CreateRuntimeError(std::move(
          llvm::formatv("\n    at {0}\nField {1} is not a bitfield",
                        ctx.expr->getExprLoc().printToString(data.Ctx.getSourceManager()), fd->getNameAsString())));
    }

    const CodeGen::CGBitFieldInfo &info =
        data.code_gen_module.getTypes().getCGRecordLayout(fd->getParent()).getBitFieldInfo(fd);

    const QualType ft = member_expr->getType();

    // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
    auto base_built_expr = BuildExpr(BuildExprCtx(member_expr->getBase(), Usage::Place, ctx.deref_force_extract,
                                                  ctx.assigned_to, ctx.string_literal_usage_kind));
    if (auto error = base_built_expr.takeError()) {
      return error;
    }
    auto src_built_expr = BuildExpr(
        BuildExprCtx(src_expr, Usage::Value, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
    if (auto error = src_built_expr.takeError()) {
      return error;
    }
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string base_addr =
        member_expr->isArrow() ? base_built_expr->final_expr : llvm::formatv("(&{0})", base_built_expr->final_expr);

    const auto is_volatile = ft.isVolatileQualified();
    const auto use_volatile =
        is_volatile && info.VolatileStorageSize != 0 && IsAAPCS(data.code_gen_module.getTypes().getTarget());

    const auto offset = use_volatile ? info.VolatileOffset : info.Offset;
    const auto storage_size = use_volatile ? info.VolatileStorageSize : info.StorageSize;
    if (storage_size > GetPointerWidth(data.Ctx)) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nBitfield storage size {1} exceeds pointer width {2}",
                                  member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), storage_size,
                                  GetPointerWidth(data.Ctx))));
    }
    const auto storage_offset = use_volatile ? info.VolatileStorageOffset : info.StorageOffset;

    // Compute the storage-unit pointer, equivalent to Dst.getBitFieldAddress()
    std::string storage_type_name = GetIntTypeName(storage_size, false);
    auto storage_ptr =
        llvm::formatv("(({0} *)((uint8_t *){1} + {2}LU))", storage_type_name, base_addr, storage_offset.getQuantity());

    // SrcVal = Builder.CreateIntCast(SrcVal, Ptr.getElementType(), /*isSigned=*/false);
    std::string src_cast_temp = GetTempVarName("bf_srccast");
    pre_stmts.push_back(
        llvm::formatv("{0} {1} = ({0}){2};", storage_type_name, src_cast_temp, src_built_expr->final_expr));
    std::string src_temp = src_cast_temp;
    // MaskedVal = SrcVal (pre-shift, pre-merge)
    std::string masked_temp = src_cast_temp;

    if (storage_size != info.Size) {
      if (storage_size <= info.Size) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nBitfield storage size {1} must be greater than bitfield size {2}",
            member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), storage_size, info.Size)));
      }

      // Val = Builder.CreateLoad(Ptr, Dst.isVolatileQualified(), "bf.load");
      std::string load_temp = GetTempVarName("bf_load");
      pre_stmts.push_back(llvm::formatv("{0} {1} {2} = *{3};", is_volatile ? "volatile " : "", storage_type_name,
                                        load_temp, storage_ptr));

      // Mask the source value as needed, unless the destination has a boolean representation.
      if (!ft->hasBooleanRepresentation()) {
        const llvm::APInt low_mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string value_temp = GetTempVarName("bf_value");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} & {3}U;", storage_type_name, value_temp, src_temp, FormatAPIntHex(low_mask)));
        src_temp = value_temp;
        masked_temp = value_temp;
      }

      // if (Offset) SrcVal = Builder.CreateShl(SrcVal, Offset, "bf.shl");
      if (offset != 0U) {
        std::string shl_temp = GetTempVarName("bf_shl");
        pre_stmts.push_back(llvm::formatv("{0} {1} = {2} << {3}U;", storage_type_name, shl_temp, src_temp, offset));
        src_temp = shl_temp;
      }

      // Val = Builder.CreateAnd(Val, ~getBitsSet(StorageSize, Offset, Offset + Size), "bf.clear");
      const llvm::APInt clear_mask = ~llvm::APInt::getBitsSet(storage_size, offset, offset + info.Size);
      std::string clear_temp = GetTempVarName("bf_clear");
      pre_stmts.push_back(
          llvm::formatv("{0} {1} = {2} & {3}U;", storage_type_name, clear_temp, load_temp, FormatAPIntHex(clear_mask)));

      // SrcVal = Builder.CreateOr(Val, SrcVal, "bf.set");
      std::string set_temp = GetTempVarName("bf_set");
      pre_stmts.push_back(llvm::formatv("{0} {1} = {2} | {3};", storage_type_name, set_temp, clear_temp, src_temp));
      src_temp = set_temp;
    } else {
      if (offset != 0) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nBitfield offset {1} must be zero when storage size {2} equals bitfield size {3}",
            member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), offset, storage_size, info.Size)));
      }
      // According to the AACPS:
      // When a volatile bit-field is written, and its container does not overlap
      // with any non-bit-field member, its container must be read exactly once
      // and written exactly once using the access width appropriate to the type
      // of the container. The two accesses are not atomic.
      if (is_volatile && IsAAPCS(data.code_gen_module.getTypes().getTarget()) &&
          data.code_gen_module.getCodeGenOpts().ForceAAPCSBitfieldLoad) {
        std::string discard_temp = GetTempVarName("AAPCS_bf_load");
        pre_stmts.push_back(
            llvm::formatv("volatile {0} {1} = *{2};\n(void){1};", storage_type_name, discard_temp, storage_ptr));
      } else {
        // the bitfield is the same size as the storage unit
        // AND the bitfield isn't volatile
        // this means we don't need to read the bitfield at all
        // we can just write to it below and be done with it
      }
    }

    // Write the new value back out: *Ptr = SrcVal
    pre_stmts.push_back(llvm::formatv("*{0} = {1};", storage_ptr, src_temp));

    // This is not needed because the usage kind is always Usage::Effect for bitfield stores, so the result is never
    // used.
    // // Return the new value of the bit-field
    // std::string result_val_temp = masked_temp;
    // // explicit sign extend the value
    // if (info.IsSigned) {
    //   assert(info.Size <= storage_size);
    //   unsigned const high_bits = storage_size - info.Size;

    //   if (high_bits != 0U) {
    //     std::string signed_type_name = GetIntTypeName(storage_size, true);

    //     // sign = 1UL << (Size - 1)
    //     llvm::APInt const sign_bit_mask = llvm::APInt::getOneBitSet(storage_size, info.Size - 1);
    //     std::string sign_temp = GetTempVarName("bf_result_sign");
    //     pre_stmts.push_back(
    //         llvm::formatv("{0} {1} = {2};", storage_type_name, sign_temp, FormatAPIntHex(sign_bit_mask)));

    //     // (int)((value ^ sign) - sign)
    //     std::string result_extend_temp = GetTempVarName("bf_result_extend");
    //     pre_stmts.push_back(llvm::formatv("{0} {1} = ({0})(({2} ^ {3}) - {3});", signed_type_name,
    //     result_extend_temp,
    //                                       result_val_temp, sign_temp));
    //     result_val_temp = result_extend_temp;
    //   }
    // }

    // // ResultVal = Builder.CreateIntCast(ResultVal, ResLTy, Info.IsSigned, "bf.result.cast");
    // std::string result_type_name = ft.getAsString();
    // std::string result_cast_temp = GetTempVarName("bf_result_cast");
    // pre_stmts.push_back(llvm::formatv("{0} {1} = ({0}){2};", result_type_name, result_cast_temp, result_val_temp));
    // std::string const result_expr = result_cast_temp;

    auto final_pre_stmts = pre_stmts | std::views::reverse | std::ranges::to<llvm::SmallVector<std::string, 4>>();
    final_pre_stmts.insert(final_pre_stmts.end(), src_built_expr->pre_stmts.begin(), src_built_expr->pre_stmts.end());
    final_pre_stmts.insert(final_pre_stmts.end(), base_built_expr->pre_stmts.begin(), base_built_expr->pre_stmts.end());

    return BuiltExpr(std::move(final_pre_stmts), "", ft);
  }

  auto BuildExpr(const pass_lower_nested_expressions::BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    auto *expr = ctx.expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const QualType final_expr_type = expr->getType();

    if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto sub_expr_usage = GetUsage(data.Ctx, sub_expr);
      if (auto error = sub_expr_usage.takeError()) {
        return std::move(error);
      }
      auto res = BuildExpr(BuildExprCtx(sub_expr, *sub_expr_usage, ctx.deref_force_extract, ctx.assigned_to,
                                        ctx.string_literal_usage_kind));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      os << "(";
      c_style_cast_expr->getTypeAsWritten().print(os, data.Ctx.getPrintingPolicy());
      os << ")";
      os << res->final_expr;

      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
      case BO_Assign: {
        // always a store operation here
        if (auto *member_expr = dyn_cast<MemberExpr>(lhs)) {
          if (member_expr->isArrow()) {
            return CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nArrow member access is not allowed in this context",
                                        member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
          }
          if (auto *field_decl = llvm::dyn_cast<clang::FieldDecl>(member_expr->getMemberDecl())) {
            if (field_decl->isBitField()) {
              const CodeGen::CGBitFieldInfo &info = data.code_gen_module.getTypes()
                                                        .getCGRecordLayout(field_decl->getParent())
                                                        .getBitFieldInfo(field_decl);
              QualType const ft = member_expr->getType();
              const auto use_volatile =
                  ft.isVolatileQualified() && IsAAPCS(data.code_gen_module.getTypes().getTarget());
              // stupid AAPCS ABI requires volatile bitfields to be loaded/stored along with their whole container...
              // note that the FIRST volatile bitfield will always have info.VolatileStorageSize == 0 so we omit the
              // check here however, in BuildStoreThroughBitFieldLValue, we check for info.VolatileStorageSize != 0
              // to determine if we should use the volatile path

              if (ctx.usage_kind != Usage::Effect) {
                return CreateRuntimeError(
                    std::move(llvm::formatv("\n    at {0}\nUsage kind must be Effect, got {1}",
                                            member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                            std::to_underlying(ctx.usage_kind))));
              }

              if (use_volatile) {
                auto built_expr =
                    BuildStoreThroughBitFieldLValue(BuildExprCtx(member_expr, Usage::Effect, ctx.deref_force_extract,
                                                                 ctx.assigned_to, ctx.string_literal_usage_kind),
                                                    rhs);
                os << built_expr->final_expr;
                pre_stmts.insert(pre_stmts.end(), built_expr->pre_stmts.begin(), built_expr->pre_stmts.end());
              } else {
                // otherwise we can use the non-volatile helpers to do the bitfield store
                auto base_built_expr =
                    BuildExpr(BuildExprCtx(member_expr->getBase(), Usage::Place, ctx.deref_force_extract,
                                           ctx.assigned_to, ctx.string_literal_usage_kind));
                if (auto error = base_built_expr.takeError()) {
                  return std::move(error);
                }
                auto rhs_usage = GetUsage(data.Ctx, rhs);
                if (auto error = rhs_usage.takeError()) {
                  return std::move(error);
                }
                auto rhs_built_expr = BuildExpr(BuildExprCtx(rhs, *rhs_usage, ctx.deref_force_extract, ctx.assigned_to,
                                                             ctx.string_literal_usage_kind));
                if (auto error = rhs_built_expr.takeError()) {
                  return std::move(error);
                }
                std::string base_addr =
                    llvm::formatv("(&{0})", base_built_expr->final_expr); // arrow member access not possible
                uint64_t start_bit = (static_cast<uint64_t>(info.StorageOffset.getQuantity()) * 8) + info.Offset;
                uint64_t end_bit = start_bit + info.Size; // info.Size = bitfield width in bits

                pre_stmts.push_back(
                    llvm::formatv("__c2pnk_set_bitfield_{0}{1}(({2}int{1}_t){3}, (uint8_t *){4}, {5}UL, {6}UL);",
                                  info.IsSigned ? "i" : "u", GetPointerWidth(data.Ctx), info.IsSigned ? "" : "u",
                                  rhs_built_expr->final_expr, base_addr, start_bit, end_bit));
                pre_stmts.insert(pre_stmts.end(), rhs_built_expr->pre_stmts.begin(), rhs_built_expr->pre_stmts.end());
                pre_stmts.insert(pre_stmts.end(), base_built_expr->pre_stmts.begin(), base_built_expr->pre_stmts.end());
                data.need_non_volatile_bitfield_helpers = true;
              }

              // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto)
              goto build_expr_end;
            }
          }
        }

        auto rhs_usage = GetUsage(data.Ctx, rhs);
        if (auto error = rhs_usage.takeError()) {
          return std::move(error);
        }
        auto rhs_res = BuildExpr(
            BuildExprCtx(rhs, *rhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_usage = GetUsage(data.Ctx, lhs);
        if (auto error = lhs_usage.takeError()) {
          return std::move(error);
        }
        auto lhs_res = BuildExpr(
            BuildExprCtx(lhs, *lhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nAssignment operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res->final_expr, rhs_res->final_expr).str());
        if (ctx.usage_kind == Usage::Value) {
          os << lhs_res->final_expr;
        }

        pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
        break;
      }

      case BO_Mul:
        [[fallthrough]];
      case BO_Div:
        [[fallthrough]];
      case BO_Rem:
        [[fallthrough]];
      case BO_Add:
        [[fallthrough]];
      case BO_Sub:
        [[fallthrough]];
      case BO_Shl:
        [[fallthrough]];
      case BO_Shr:
        [[fallthrough]];
      case BO_LT:
        [[fallthrough]];
      case BO_GT:
        [[fallthrough]];
      case BO_LE:
        [[fallthrough]];
      case BO_GE:
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

        auto rhs_usage = GetUsage(data.Ctx, rhs);
        if (auto error = rhs_usage.takeError()) {
          return std::move(error);
        }
        auto rhs_res = BuildExpr(
            BuildExprCtx(rhs, *rhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_usage = GetUsage(data.Ctx, lhs);
        if (auto error = lhs_usage.takeError()) {
          return std::move(error);
        }
        auto lhs_res = BuildExpr(
            BuildExprCtx(lhs, *lhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nBinary operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        // don't return a pre stmt no matter what since these can be arbitrarily nested
        os << llvm::formatv("({0} {1} {2})", lhs_res->final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res->final_expr);

        pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
        break;
      }

      default: {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nUnhandled binary operator {1}",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    BinaryOperator::getOpcodeStr(binary_operator->getOpcode()).str())));
        break;
      }
      }
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();

      switch (unary_operator->getOpcode()) {
      case UO_Deref: {
        auto usage_kind = ctx.deref_force_extract ? Usage::Value : ctx.usage_kind;
        auto sub_expr_usage = GetUsage(data.Ctx, sub_expr);
        if (auto error = sub_expr_usage.takeError()) {
          return std::move(error);
        }
        auto res =
            BuildExpr(BuildExprCtx(sub_expr, *sub_expr_usage, true, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return std::move(error);
        }

        if (usage_kind == Usage::Place) {
          os << "*" << res->final_expr;
        } else {
          // extract into temp var
          std::string const tmp_var_name = GetTempVarName("Deref");

          auto decl_type = final_expr_type;
          if (final_expr_type->isArrayType()) {
            // Can't copy-initialize an array object. Declare a pointer to the array's element type instead
            const auto *array_type = data.Ctx.getAsArrayType(final_expr_type);
            decl_type = data.Ctx.getPointerType(array_type->getElementType());
          }
          pre_stmts.push_back(llvm::formatv("{0} = *{1};", PrintType(decl_type, tmp_var_name), res->final_expr).str());
          os << tmp_var_name;
        }

        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      case UO_AddrOf:
        [[fallthrough]];
      case UO_Plus:
        [[fallthrough]];
      case UO_Minus:
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
        auto sub_expr_usage = GetUsage(data.Ctx, sub_expr);
        if (auto error = sub_expr_usage.takeError()) {
          return std::move(error);
        }
        auto res = BuildExpr(BuildExprCtx(sub_expr, *sub_expr_usage, ctx.deref_force_extract, ctx.assigned_to,
                                          ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return std::move(error);
        }
        os << llvm::formatv("{0}{1}", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res->final_expr);
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      default: {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nUnhandled unary operator {1}",
                                    unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str())));
        break;
      }
      }
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        auto arg_usage = GetUsage(data.Ctx, arg);
        if (auto error = arg_usage.takeError()) {
          return std::move(error);
        }
        auto res = BuildExpr(
            BuildExprCtx(arg, *arg_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return std::move(error);
        }
        arg_built_exprs.push_back(std::move(*res));
      }

      if (call_expr->getDirectCallee() == nullptr) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\ngetDirectCallee returned nullptr for a call expression",
                                    call_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      os << llvm::formatv("{0}({1})", call_expr->getDirectCallee()->getName().str(),
                          llvm::join(arg_built_exprs | std::views::transform([](const BuiltExpr &e) -> std::string {
                                       return e.final_expr;
                                     }),
                                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      // writes are handled by the assignment operator
      if (IsRead(member_expr)) {
        if (auto *field_decl = llvm::dyn_cast<clang::FieldDecl>(member_expr->getMemberDecl())) {
          if (field_decl->isBitField()) {
            if (member_expr->isArrow()) {
              return CreateRuntimeError(
                  std::move(llvm::formatv("\n    at {0}\nArrow member access is not allowed in this context",
                                          member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
            }

            const CodeGen::CGBitFieldInfo &info =
                data.code_gen_module.getTypes().getCGRecordLayout(field_decl->getParent()).getBitFieldInfo(field_decl);
            QualType const ft = member_expr->getType();
            const auto use_volatile = ft.isVolatileQualified() && IsAAPCS(data.code_gen_module.getTypes().getTarget());
            // stupid AAPCS ABI requires volatile bitfields to be loaded/stored along with their whole container...
            // note that the FIRST volatile bitfield will always have info.VolatileStorageSize == 0 so we omit the
            // check here however, in BuildLoadOfBitFieldLValue, we check for info.VolatileStorageSize != 0 to
            // determine if we should use the volatile path

            if (use_volatile) {
              auto member_expr_usage = GetUsage(data.Ctx, member_expr);
              if (auto error = member_expr_usage.takeError()) {
                return std::move(error);
              }
              auto res =
                  BuildLoadOfBitFieldLValue(BuildExprCtx(member_expr, *member_expr_usage, ctx.deref_force_extract,
                                                         ctx.assigned_to, ctx.string_literal_usage_kind));
              if (auto error = res.takeError()) {
                return std::move(error);
              }
              os << res->final_expr;
              pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
            } else {
              // otherwise fallback to helper functions

              // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
              auto base_usage = GetUsage(data.Ctx, member_expr->getBase());
              if (auto error = base_usage.takeError()) {
                return std::move(error);
              }
              auto base_built_expr =
                  BuildExpr(BuildExprCtx(member_expr->getBase(), *base_usage, ctx.deref_force_extract, ctx.assigned_to,
                                         ctx.string_literal_usage_kind));
              if (auto error = base_built_expr.takeError()) {
                return std::move(error);
              }
              std::string base_addr =
                  llvm::formatv("(&{0})", base_built_expr->final_expr); // arrow member access not possible
              uint64_t start_bit = (static_cast<uint64_t>(info.StorageOffset.getQuantity()) * 8) + info.Offset;
              uint64_t end_bit = start_bit + info.Size; // info.Size = bitfield width in bits
              os << llvm::formatv("__c2pnk_get_bitfield_{0}{1}((const uint8_t *){2}, {3}UL, {4}UL)",
                                  info.IsSigned ? "i" : "u", GetPointerWidth(data.Ctx), base_addr, start_bit, end_bit);
              pre_stmts.insert(pre_stmts.end(), base_built_expr->pre_stmts.begin(), base_built_expr->pre_stmts.end());
              data.need_non_volatile_bitfield_helpers = true;
            }

            // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto)
            goto build_expr_end;
          }
        }
      }
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto)
      goto build_expr_else;
    } else {
    build_expr_else:
      if (auto err = PrintSourceText(os, expr, data.Ctx)) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nFailed to print source text for expression: {1}",
            expr->getExprLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
      }
    }

  build_expr_end:
    os.flush();
    return BuiltExpr(pre_stmts, final_expr, final_expr_type);
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    if (data.error) {
      return false;
    }

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
          auto init_expr_usage = GetUsage(data.Ctx, init_expr);
          if (auto error = init_expr_usage.takeError()) {
            data.error = std::move(error);
            return false;
          }
          auto res = BuildExpr(BuildExprCtx(
              init_expr, *init_expr_usage, false,
              std::make_optional(std::make_pair(var_decl->getNameAsString(), var_decl->getType().getCanonicalType())),
              std::nullopt));
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
    if (stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(stmt->getBeginLoc()))) {
      return true;
    }

    if (auto *expr = dyn_cast<Expr>(stmt)) {
      expr = expr->IgnoreParenImpCasts();

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      auto res = BuildExpr(BuildExprCtx(expr, Usage::Effect, false, std::nullopt, std::nullopt));
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
  // this code is incredibly fucked...
  // the codegen stuff in clang is all considered "private" and not part of the public clang C++ API
  // but the headers can just be downloaded and included anyway
  // all the compiled object code is available already...
  // this means these headers are incredibly brittle :skull:
  CodeGenOptions const code_gen_options;
  llvm::LLVMContext llvm_ctx;
  auto layout_probe = std::make_unique<llvm::Module>("layout_probe", llvm_ctx);
  CodeGen::CodeGenModule code_gen_module(Ctx, ci.getVirtualFileSystemPtr(), ci.getHeaderSearchOpts(),
                                         ci.getPreprocessorOpts(), code_gen_options, *layout_probe,
                                         ci.getDiagnostics());

  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .code_gen_module = code_gen_module, .pa_ctx = ps_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  if (data.need_non_volatile_bitfield_helpers) {
    replacements.emplace_back(Ctx.getSourceManager(),
                              Ctx.getSourceManager().getLocForStartOfFile(Ctx.getSourceManager().getMainFileID()), 0,
                              llvm::formatv(non_volatile_bitfield_helpers, GetPointerWidth(Ctx)).str());
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
} // namespace pancake::pass_lower_bitfield_ops

namespace pancake::pass_simplify_addrof_deref {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(
      unaryOperator(isExpansionInMainFile(), hasOperatorName("&"),
                    hasUnaryOperand(ignoringParenImpCasts(
                        unaryOperator(hasOperatorName("*"), hasUnaryOperand(expr().bind("expr"))).bind("deref"))))
          .bind("addr_of"),
      changeTo(node("addr_of"), cat(node("expr"))));
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
} // namespace pancake::pass_simplify_addrof_deref
