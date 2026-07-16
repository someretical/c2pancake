#include "Pass_LowerBitfieldOps.h"

#include "third_party/Address.h"
#include "third_party/CGRecordLayout.h"
#include "third_party/CodeGenFunction.h"
#include "third_party/CodeGenModule.h"
#include "third_party/CodeGenTypes.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecordLayout.h>
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
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::transformer;

// TODO hoist nested struct defs into the same scope as the outermost one

namespace pancake::pass_lower_arrow_accesses {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineActionCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  size_t tmp_var_counter = 0;
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

  struct BuiltExpr {
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string final_expr;
    QualType final_expr_type;
  };

  struct BuiltExprCtx {
    size_t depth = 0;
    bool encountered_top_most_arrow_access = false;
  };

  auto BuildExpr(Expr *expr, const BuiltExprCtx ctx) -> BuiltExpr {
    expr = expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    QualType final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(decl_ref_expr->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(integer_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *floating_literal = dyn_cast<FloatingLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(floating_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(character_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *string_literal = dyn_cast<StringLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(string_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
      case BO_Assign: {
        auto rhs_res = BuildExpr(
            rhs, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});
        auto lhs_res = BuildExpr(
            lhs, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});

        if (ctx.depth == 0) {
          // semi-colon is added by the caller
          os << llvm::formatv("{0} = {1}", lhs_res.final_expr, rhs_res.final_expr);
        } else {
          // we need to hoist this before
          pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res.final_expr, rhs_res.final_expr).str());
          os << lhs_res.final_expr;
        }

        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());

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
        auto rhs_res = BuildExpr(
            rhs, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});
        auto lhs_res = BuildExpr(
            lhs, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});

        // recursion level doesn't matter
        os << llvm::formatv("({0} {1} {2})", lhs_res.final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res.final_expr);

        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());
        break;
      }

      default: {
        llvm::outs() << "Unhandled binary operator: " << BinaryOperator::getOpcodeStr(binary_operator->getOpcode())
                     << "\n";
        llvm_unreachable("Unhandled binary operator");
        break;
      }
      }
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      auto res = BuildExpr(sub_expr, {.depth = ctx.depth + 1,
                                      .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});

      switch (unary_operator->getOpcode()) {
      case UO_AddrOf:
        [[fallthrough]];
      case UO_Deref:
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
        os << llvm::formatv("{0}({1})", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res.final_expr);
        break;
      }

      default: {
        llvm_unreachable("Unhandled unary operator");
        break;
      }
      }

      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        arg_built_exprs.push_back(BuildExpr(
            arg, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access}));
      }
      // TODO
      // it's possible for getDirectCallee to return nullptr, but I don't know what to do in that case...
      os << llvm::formatv("{0}({1})", call_expr->getDirectCallee()->getName().str(),
                          llvm::join(arg_built_exprs | std::views::transform([](const BuiltExpr &e) -> std::string {
                                       return e.final_expr;
                                     }),
                                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *array_subscript_expr = dyn_cast<ArraySubscriptExpr>(expr)) {
      auto *idx = array_subscript_expr->getIdx();
      auto *base = array_subscript_expr->getBase();

      auto res = BuildExpr(
          idx, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});
      os << llvm::formatv("{0}[{1}]",
                          Lexer::getSourceText(CharSourceRange::getTokenRange(base->getSourceRange()),
                                               data.Ctx.getSourceManager(), data.Ctx.getLangOpts()),
                          res.final_expr);
      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      auto *base_expr = member_expr->getBase()->IgnoreParenImpCasts();
      auto member_src = Lexer::getSourceText(CharSourceRange::getTokenRange(member_expr->getMemberLoc()),
                                             data.Ctx.getSourceManager(), data.Ctx.getLangOpts())
                            .str();

      if (member_expr->isLValue()) {
        if (member_expr->isArrow()) {
          // if this is the top-most isArrow, we CANNOT hoist to a temporary
          if (ctx.encountered_top_most_arrow_access) {
            std::string tmp_var_name = GetTempVarName("ArrowAccess");
            auto built_expr = BuildExpr(base_expr, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = true});
            pre_stmts.push_back(llvm::formatv("{0} = {1}->{2};", PrintType(final_expr_type, tmp_var_name),
                                              built_expr.final_expr, member_src)
                                    .str());
            os << tmp_var_name;
            pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
          } else {
            auto built_expr = BuildExpr(base_expr, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = true});
            os << llvm::formatv("{0}->{1}", built_expr.final_expr, member_src);
            pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
          }
        } else {
          // . accesses MUST be preserved if memberexpr is used as an lvalue
          auto built_expr =
              BuildExpr(base_expr, {.depth = ctx.depth + 1,
                                    .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});
          os << llvm::formatv("{0}.{1}", built_expr.final_expr, member_src);
          pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
        }
      } else {
        if (member_expr->isArrow()) {
          // if this is the top-most isArrow, we can avoid hoisting a temporary
          if (ctx.encountered_top_most_arrow_access) {
            std::string tmp_var_name = GetTempVarName("ArrowAccess");
            auto built_expr = BuildExpr(base_expr, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = true});
            pre_stmts.push_back(llvm::formatv("{0} = {1}->{2};", PrintType(final_expr_type, tmp_var_name),
                                              built_expr.final_expr, member_src)
                                    .str());
            os << tmp_var_name;
            pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
          } else {
            auto built_expr = BuildExpr(base_expr, {.depth = ctx.depth + 1, .encountered_top_most_arrow_access = true});
            os << llvm::formatv("{0}->{1}", built_expr.final_expr, member_src);
            pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
          }
        } else {
          // . accesses should be preserved since they are just an offset which can calculated easily
          auto built_expr =
              BuildExpr(base_expr, {.depth = ctx.depth + 1,
                                    .encountered_top_most_arrow_access = ctx.encountered_top_most_arrow_access});
          os << llvm::formatv("{0}.{1}", built_expr.final_expr, member_src);
          pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
        }
      }
    } else {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), data.Ctx.getSourceManager(),
                                 data.Ctx.getLangOpts());
    }

    os.flush();
    return {.pre_stmts = std::move(pre_stmts),
            .final_expr = std::move(replacement_text),
            .final_expr_type = final_expr_type};
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    auto &sm = data.Ctx.getSourceManager();
    if (declStmt == nullptr || sm.isInSystemHeader(sm.getSpellingLoc(declStmt->getBeginLoc()))) {
      return true;
    }

    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);

    for (auto *decl : declStmt->decls()) {
      if (auto *var_decl = dyn_cast<VarDecl>(decl)) {
        auto *init_expr = var_decl->getInit();
        if (init_expr == nullptr) {
          continue;
        }

        auto res =
            BuildExpr(init_expr->IgnoreParenImpCasts(), {.depth = 0, .encountered_top_most_arrow_access = false});
        os << llvm::join(res.pre_stmts | std::views::reverse, "\n");
        os << llvm::formatv("{0} = {1};", PrintType(var_decl->getType(), var_decl->getName()), res.final_expr);
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
    if (stmt == nullptr || sm.isInSystemHeader(sm.getSpellingLoc(stmt->getBeginLoc()))) {
      return true;
    }
    if (auto *expr = dyn_cast<Expr>(stmt)) {
      expr = expr->IgnoreParenImpCasts();

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      auto res = BuildExpr(expr, {.depth = 0, .encountered_top_most_arrow_access = false});
      os << llvm::join(res.pre_stmts | std::views::reverse, "\n");
      // for some reason we don't need a ; after this...
      os << llvm::formatv("{0}", res.final_expr);
      os.flush();

      if (!replacement_text.empty()) {
        data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                       CharSourceRange::getTokenRange(stmt->getSourceRange()), replacement_text,
                                       data.Ctx.getLangOpts());
      }
      return true;
    }

    return RecursiveASTVisitor::TraverseStmt(stmt);
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = pa_ctx, .replacements = replacements};
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

  pa_ctx.failure_mode = FailureMode::Success;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_lower_arrow_accesses

namespace pancake::pass_lower_bitfield_ops {
namespace {
// This helper code facilitates non-volatile bitfield operations.
const char *NON_VOLATILE_BITFIELD_HELPERS =
    R"(/* c2pancake generated code start: helpers for non-volatile bitfield operations */
#include <stdint.h>

static inline uint64_t __c2pnk_get_bit_u64(uint64_t value, uint64_t bit) { return ((value >> bit) & 1ULL) != 0; }

static inline void __c2pnk_set_bit(uint8_t *byte, uint64_t bit) {
  uint64_t val = (uint64_t)*byte;
  // truncation
  *byte = (uint8_t)(val | (uint64_t)(1ULL << bit));
}

static inline void __c2pnk_clear_bit(uint8_t *byte, uint64_t bit) {
  uint64_t val = (uint64_t)*byte;
  // truncation
  *byte = (uint8_t)(val & ~(1ULL << bit));
}

/* [lhs_bit, rhs_bit) */
static void __c2pnk_set_bitfield_u64(uint64_t value, uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t width = rhs_bit - lhs_bit;

  uint64_t i = 0;
  while (i < width) {
    uint64_t bit_index = lhs_bit + i;
    uint8_t *byte = &field[bit_index >> 3]; // / 8

    uint64_t cond = __c2pnk_get_bit_u64(value, i);
    uint64_t index = bit_index & 7; // % 8
    if (cond) {
      __c2pnk_set_bit(byte, index);
    } else {
      __c2pnk_clear_bit(byte, index);
    }

    i = i + 1;
  }
}

/* [lhs_bit, rhs_bit) */
static uint64_t __c2pnk_get_bitfield_u64(const uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t value = 0;
  uint64_t width = rhs_bit - lhs_bit;

  uint64_t i = 0;
  while (i < width) {
    uint64_t bit_index = lhs_bit + i;

    uint64_t byte = (uint64_t)field[bit_index >> 3]; // / 8
    uint64_t index = bit_index & 7;                  // % 8
    uint64_t mask = (uint64_t)(1ULL << index);

    if (byte & mask) {
      value = value | (1ULL << i);
    }

    i = i + 1;
  }

  return value;
}

/* [lhs_bit, rhs_bit) */
static int64_t __c2pnk_get_bitfield_i64(const uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t value = __c2pnk_get_bitfield_u64(field, lhs_bit, rhs_bit);

  uint64_t width = rhs_bit - lhs_bit;

  /* manual sign-extend if the extracted field is narrower than 64 bits */
  if (width < 64) {
    uint64_t sign = 1ULL << (width - 1);
    return (int64_t)((value ^ sign) - sign);
  }

  return (int64_t)value;
}

/* [lhs_bit, rhs_bit) */
static void __c2pnk_set_bitfield_i64(int64_t value, uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  __c2pnk_set_bitfield_u64((uint64_t)value, field, lhs_bit, rhs_bit);
}
/* c2pancake generated code end: helpers for non-volatile bitfield operations */

)";

struct WorkerData {
  ASTContext &Ctx;
  CodeGen::CodeGenModule &code_gen_module;
  PipelineActionCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  size_t tmp_var_counter = 0;
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
      auto parents = data.Ctx.getParents(*stmt);
      if (parents.empty())
        return false;

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

      However, when a loaded value undergoes further conversion (integer promotion, usual arithmetic conversion, pointer
      decay of the result, etc.), those additional ImplicitCastExpr nodes wrap around the LValueToRValue cast so it's
      not a problem in this case.
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

  struct BuiltExpr {
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string final_expr;
    QualType final_expr_type;
  };

  struct BuiltExprCtx {
    size_t depth = 0;
  };

  // Equivalent to
  // https://clang.llvm.org/doxygen/classclang_1_1CodeGen_1_1CodeGenFunction.html#abce29203390acfa7bfcda7d0c9101629
  // We only use this function for VOLATILE bitfields since the loads/stores generated should be always 8,16,32,64
  // If this ever changes, then we're fucked because clang IR can generate arbitrary width loads/stores
  auto BuildLoadOfBitFieldLValue(const MemberExpr *member_expr, const BuiltExprCtx &ctx) -> BuiltExpr {
    const FieldDecl *fd = cast<FieldDecl>(member_expr->getMemberDecl());
    assert(fd->isBitField());

    const CodeGen::CGBitFieldInfo &info =
        data.code_gen_module.getTypes().getCGRecordLayout(fd->getParent()).getBitFieldInfo(fd);

    QualType ft = member_expr->getType();

    // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
    BuiltExpr base_built_expr = BuildExpr(member_expr->getBase(), {.depth = ctx.depth + 1});
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string base_addr =
        member_expr->isArrow() ? base_built_expr.final_expr : llvm::formatv("(&{0})", base_built_expr.final_expr);

    const auto is_volatile = ft.isVolatileQualified();
    const auto use_volatile =
        is_volatile && info.VolatileStorageSize != 0 && IsAAPCS(data.code_gen_module.getTypes().getTarget());

    const auto offset = use_volatile ? info.VolatileOffset : info.Offset;
    const auto storage_size = use_volatile ? info.VolatileStorageSize : info.StorageSize;
    const auto storage_offset = use_volatile ? info.VolatileStorageOffset : info.StorageOffset;

    // Compute the storage-unit pointer, equivalent to LV.getBitFieldAddress()
    std::string storage_type_name = GetIntTypeName(storage_size, false);
    auto storage_ptr =
        llvm::formatv("(({0} *)((char *){1} + {2}))", storage_type_name, base_addr, storage_offset.getQuantity());

    std::string load_temp = GetTempVarName("bf_load");
    pre_stmts.push_back(llvm::formatv("{0} {1} {2} = *{3};", is_volatile ? "volatile " : "", storage_type_name,
                                      load_temp, storage_ptr));
    std::string current_temp = load_temp;

    if (info.IsSigned) {
      assert(static_cast<unsigned>(offset + info.Size) <= storage_size);
      std::string signed_type_name = GetIntTypeName(storage_size, true);

      // Extract the field: (current_temp >> offset) & low_bits_mask(Size)
      // (equivalent to what the shl/ashr pair achieved implicitly, but as a plain unsigned extraction)
      std::string extracted_temp = current_temp;
      if (offset != 0U) {
        std::string lshr_temp = GetTempVarName("bf_lshr");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} >> {3};", storage_type_name, lshr_temp, extracted_temp, offset));
        extracted_temp = lshr_temp;
      }
      if (static_cast<unsigned>(offset) + info.Size < storage_size) {
        llvm::APInt low_mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string mask_temp = GetTempVarName("bf_mask");
        pre_stmts.push_back(llvm::formatv("{0} {1} = {2} & {3};", storage_type_name, mask_temp, extracted_temp,
                                          FormatAPIntHex(low_mask)));
        extracted_temp = mask_temp;
      }
      current_temp = extracted_temp;

      // Sign-extend: sign = 1ULL << (Size - 1); (T)((value ^ sign) - sign)
      llvm::APInt sign_bit_mask = llvm::APInt::getOneBitSet(storage_size, info.Size - 1);
      std::string sign_temp = GetTempVarName("bf_sign");
      pre_stmts.push_back(llvm::formatv("{0} {1} = {2};", storage_type_name, sign_temp, FormatAPIntHex(sign_bit_mask)));

      std::string signed_temp = GetTempVarName("bf_signed");
      pre_stmts.push_back(
          llvm::formatv("{0} {1} = ({0})(({2} ^ {3}) - {3});", signed_type_name, signed_temp, current_temp, sign_temp));
      current_temp = signed_temp;
    } else {
      // Val = Builder.CreateLShr(Val, Offset, "bf.lshr");
      if (offset != 0U) {
        std::string lshr_temp = GetTempVarName("bf_lshr");
        pre_stmts.push_back(llvm::formatv("{0} {1} = {2} >> {3};", storage_type_name, lshr_temp, current_temp, offset));
        current_temp = lshr_temp;
      }
      // Val = Builder.CreateAnd(Val, getLowBitsSet(StorageSize, Size), "bf.clear");
      if (static_cast<unsigned>(offset) + info.Size < storage_size) {
        llvm::APInt mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string clear_temp = GetTempVarName("bf_clear");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} & {3};", storage_type_name, clear_temp, current_temp, FormatAPIntHex(mask)));
        current_temp = clear_temp;
      }
    }

    // Val = Builder.CreateIntCast(Val, ResLTy, IsSigned, "bf.cast");
    std::string result_type_name = ft.getAsString();
    std::string cast_temp = GetTempVarName("bf_cast");
    pre_stmts.push_back(
        llvm::formatv("{0} {1} = ({2}){3};", result_type_name, cast_temp, result_type_name, current_temp));

    auto final_pre_stmts = pre_stmts | std::views::reverse | std::ranges::to<llvm::SmallVector<std::string, 4>>();
    final_pre_stmts.insert(final_pre_stmts.end(), base_built_expr.pre_stmts.begin(), base_built_expr.pre_stmts.end());
    return {.pre_stmts = std::move(final_pre_stmts), .final_expr = cast_temp, .final_expr_type = ft};
  }

  auto BuildStoreThroughBitFieldLValue(const MemberExpr *member_expr, Expr *src_expr, const BuiltExprCtx &ctx)
      -> BuiltExpr {
    const FieldDecl *fd = cast<FieldDecl>(member_expr->getMemberDecl());
    assert(fd->isBitField());

    const CodeGen::CGBitFieldInfo &info =
        data.code_gen_module.getTypes().getCGRecordLayout(fd->getParent()).getBitFieldInfo(fd);

    QualType ft = member_expr->getType();

    // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
    BuiltExpr base_built_expr = BuildExpr(member_expr->getBase(), {.depth = ctx.depth + 1});
    BuiltExpr src_built_expr = BuildExpr(src_expr, {.depth = ctx.depth + 1});
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string base_addr =
        member_expr->isArrow() ? base_built_expr.final_expr : llvm::formatv("(&{0})", base_built_expr.final_expr);

    llvm::outs() << "processing bitfield store: "
                 << member_expr->getSourceRange().printToString(data.Ctx.getSourceManager()) << "\n";

    const auto is_volatile = ft.isVolatileQualified();
    const auto use_volatile =
        is_volatile && info.VolatileStorageSize != 0 && IsAAPCS(data.code_gen_module.getTypes().getTarget());

    const auto offset = use_volatile ? info.VolatileOffset : info.Offset;
    const auto storage_size = use_volatile ? info.VolatileStorageSize : info.StorageSize;
    const auto storage_offset = use_volatile ? info.VolatileStorageOffset : info.StorageOffset;

    // Compute the storage-unit pointer, equivalent to Dst.getBitFieldAddress()
    std::string storage_type_name = GetIntTypeName(storage_size, false);
    auto storage_ptr =
        llvm::formatv("(({0} *)((char *){1} + {2}))", storage_type_name, base_addr, storage_offset.getQuantity());

    // SrcVal = Builder.CreateIntCast(SrcVal, Ptr.getElementType(), /*isSigned=*/false);
    std::string src_cast_temp = GetTempVarName("bf_srccast");
    pre_stmts.push_back(
        llvm::formatv("{0} {1} = ({0}){2};", storage_type_name, src_cast_temp, src_built_expr.final_expr));
    std::string src_temp = src_cast_temp;
    // MaskedVal = SrcVal (pre-shift, pre-merge)
    std::string masked_temp = src_cast_temp;

    if (storage_size != info.Size) {
      assert(storage_size > info.Size && "Invalid bitfield size.");

      // Val = Builder.CreateLoad(Ptr, Dst.isVolatileQualified(), "bf.load");
      std::string load_temp = GetTempVarName("bf_load");
      pre_stmts.push_back(llvm::formatv("{0} {1} {2} = *{3};", is_volatile ? "volatile " : "", storage_type_name,
                                        load_temp, storage_ptr));

      // Mask the source value as needed, unless the destination has a boolean representation.
      if (!ft->hasBooleanRepresentation()) {
        llvm::APInt low_mask = llvm::APInt::getLowBitsSet(storage_size, info.Size);
        std::string value_temp = GetTempVarName("bf_value");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} & {3};", storage_type_name, value_temp, src_temp, FormatAPIntHex(low_mask)));
        src_temp = value_temp;
        masked_temp = value_temp;
      }

      // if (Offset) SrcVal = Builder.CreateShl(SrcVal, Offset, "bf.shl");
      if (offset != 0U) {
        std::string shl_temp = GetTempVarName("bf_shl");
        pre_stmts.push_back(llvm::formatv("{0} {1} = {2} << {3};", storage_type_name, shl_temp, src_temp, offset));
        src_temp = shl_temp;
      }

      // Val = Builder.CreateAnd(Val, ~getBitsSet(StorageSize, Offset, Offset + Size), "bf.clear");
      llvm::APInt clear_mask = ~llvm::APInt::getBitsSet(storage_size, offset, offset + info.Size);
      std::string clear_temp = GetTempVarName("bf_clear");
      pre_stmts.push_back(
          llvm::formatv("{0} {1} = {2} & {3};", storage_type_name, clear_temp, load_temp, FormatAPIntHex(clear_mask)));

      // SrcVal = Builder.CreateOr(Val, SrcVal, "bf.set");
      std::string set_temp = GetTempVarName("bf_set");
      pre_stmts.push_back(llvm::formatv("{0} {1} = {2} | {3};", storage_type_name, set_temp, clear_temp, src_temp));
      src_temp = set_temp;
    } else {
      assert(offset == 0);
      // According to the AACPS:
      // When a volatile bit-field is written, and its container does not overlap
      // with any non-bit-field member, its container must be read exactly once
      // and written exactly once using the access width appropriate to the type
      // of the container. The two accesses are not atomic.
      if (is_volatile && IsAAPCS(data.code_gen_module.getTypes().getTarget()) &&
          data.code_gen_module.getCodeGenOpts().ForceAAPCSBitfieldLoad) {
        std::string discard_temp = GetTempVarName("AAPCS_bf_load");
        pre_stmts.push_back(llvm::formatv("volatile {0} {1} = *{2};", storage_type_name, discard_temp, storage_ptr));
        pre_stmts.push_back(llvm::formatv("(void){0};", discard_temp));
      }
    }

    // Write the new value back out: *Ptr = SrcVal
    pre_stmts.push_back(llvm::formatv("*{0} = {1};", storage_ptr, src_temp));

    // Return the new value of the bit-field
    std::string result_val_temp = masked_temp;
    // explicit sign extend the value
    if (info.IsSigned) {
      assert(info.Size <= storage_size);
      unsigned high_bits = storage_size - info.Size;

      if (high_bits != 0U) {
        std::string signed_type_name = GetIntTypeName(storage_size, true);

        // sign = 1ULL << (Size - 1)
        llvm::APInt sign_bit_mask = llvm::APInt::getOneBitSet(storage_size, info.Size - 1);
        std::string sign_temp = GetTempVarName("bf_result_sign");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2};", storage_type_name, sign_temp, FormatAPIntHex(sign_bit_mask)));

        // (int)((value ^ sign) - sign)
        std::string result_extend_temp = GetTempVarName("bf_result_extend");
        pre_stmts.push_back(llvm::formatv("{0} {1} = ({0})(({2} ^ {3}) - {3});", signed_type_name, result_extend_temp,
                                          result_val_temp, sign_temp));
        result_val_temp = result_extend_temp;
      }
    }

    // ResultVal = Builder.CreateIntCast(ResultVal, ResLTy, Info.IsSigned, "bf.result.cast");
    std::string result_type_name = ft.getAsString();
    std::string result_cast_temp = GetTempVarName("bf_result_cast");
    pre_stmts.push_back(llvm::formatv("{0} {1} = ({0}){2};", result_type_name, result_cast_temp, result_val_temp));
    std::string result_expr = result_cast_temp;

    auto final_pre_stmts = pre_stmts | std::views::reverse | std::ranges::to<llvm::SmallVector<std::string, 4>>();
    final_pre_stmts.insert(final_pre_stmts.end(), src_built_expr.pre_stmts.begin(), src_built_expr.pre_stmts.end());
    final_pre_stmts.insert(final_pre_stmts.end(), base_built_expr.pre_stmts.begin(), base_built_expr.pre_stmts.end());

    return {.pre_stmts = std::move(final_pre_stmts), .final_expr = result_expr, .final_expr_type = ft};
  }

  auto BuildExpr(Expr *expr, const BuiltExprCtx ctx) -> BuiltExpr {
    expr = expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    QualType final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(decl_ref_expr->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(integer_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *floating_literal = dyn_cast<FloatingLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(floating_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(character_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *string_literal = dyn_cast<StringLiteral>(expr)) {
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(string_literal->getSourceRange()),
                                 data.Ctx.getSourceManager(), data.Ctx.getLangOpts());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
      case BO_Assign: {
        // always a store operation here
        if (auto *member_expr = dyn_cast<MemberExpr>(lhs)) {
          if (auto *field_decl = llvm::dyn_cast<clang::FieldDecl>(member_expr->getMemberDecl())) {
            if (field_decl->isBitField()) {
              const CodeGen::CGBitFieldInfo &info = data.code_gen_module.getTypes()
                                                        .getCGRecordLayout(field_decl->getParent())
                                                        .getBitFieldInfo(field_decl);
              QualType ft = member_expr->getType();
              const auto use_volatile =
                  ft.isVolatileQualified() && IsAAPCS(data.code_gen_module.getTypes().getTarget());
              // stupid AAPCS ABI requires volatile bitfields to be loaded/stored along with their whole container...
              // note that the FIRST volatile bitfield will always have info.VolatileStorageSize == 0 so we omit the
              // check here however, in BuildStoreThroughBitFieldLValue, we check for info.VolatileStorageSize != 0 to
              // determine if we should use the volatile path
              if (use_volatile) {
                auto built_expr = BuildStoreThroughBitFieldLValue(member_expr, rhs, {.depth = ctx.depth + 1});
                os << built_expr.final_expr;
                pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
              } else {
                // otherwise we can use the non-volatile helpers to do the bitfield store
                BuiltExpr base_built_expr = BuildExpr(member_expr->getBase(), {.depth = ctx.depth + 1});
                BuiltExpr rhs_built_expr = BuildExpr(rhs, {.depth = ctx.depth + 1});
                llvm::SmallVector<std::string, 4> pre_stmts;
                std::string base_addr = member_expr->isArrow() ? base_built_expr.final_expr
                                                               : llvm::formatv("(&{0})", base_built_expr.final_expr);
                uint64_t start_bit = ((uint64_t)(info.StorageOffset.getQuantity()) * 8) + info.Offset;
                uint64_t end_bit = start_bit + info.Size; // info.Size = bitfield width in bits

                os << llvm::formatv("__c2pnk_set_bitfield_{0}(({1}){2}, (uint8_t *){3}, {4}, {5})",
                                    info.IsSigned ? "i64" : "u64", info.IsSigned ? "int64_t" : "uint64_t",
                                    rhs_built_expr.final_expr, base_addr, start_bit, end_bit);
                pre_stmts.insert(pre_stmts.end(), rhs_built_expr.pre_stmts.begin(), rhs_built_expr.pre_stmts.end());
                pre_stmts.insert(pre_stmts.end(), base_built_expr.pre_stmts.begin(), base_built_expr.pre_stmts.end());
              }

              goto build_expr_end;
            }
          }
        }

        auto rhs_res = BuildExpr(rhs, {.depth = ctx.depth + 1});
        auto lhs_res = BuildExpr(lhs, {.depth = ctx.depth + 1});

        if (ctx.depth == 0) {
          // semi-colon is added by the caller
          os << llvm::formatv("{0} = {1}", lhs_res.final_expr, rhs_res.final_expr);
        } else {
          // we need to hoist this before
          pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res.final_expr, rhs_res.final_expr).str());
          os << lhs_res.final_expr;
        }

        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());

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
        auto rhs_res = BuildExpr(rhs, {.depth = ctx.depth + 1});
        auto lhs_res = BuildExpr(lhs, {.depth = ctx.depth + 1});

        // recursion level doesn't matter
        os << llvm::formatv("({0} {1} {2})", lhs_res.final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res.final_expr);

        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());
        break;
      }

      default: {
        llvm::outs() << "Unhandled binary operator: " << BinaryOperator::getOpcodeStr(binary_operator->getOpcode())
                     << "\n";
        llvm_unreachable("Unhandled binary operator");
        break;
      }
      }
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      auto res = BuildExpr(sub_expr, {.depth = ctx.depth + 1});

      switch (unary_operator->getOpcode()) {
      case UO_AddrOf:
        [[fallthrough]];
      case UO_Deref:
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
        os << llvm::formatv("{0}({1})", UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str(), res.final_expr);
        break;
      }

      default: {
        llvm_unreachable("Unhandled unary operator");
        break;
      }
      }

      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        arg_built_exprs.push_back(BuildExpr(arg, {.depth = ctx.depth + 1}));
      }
      // TODO
      // it's possible for getDirectCallee to return nullptr, but I don't know what to do in that case...
      os << llvm::formatv("{0}({1})", call_expr->getDirectCallee()->getName().str(),
                          llvm::join(arg_built_exprs | std::views::transform([](const BuiltExpr &e) -> std::string {
                                       return e.final_expr;
                                     }),
                                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *array_subscript_expr = dyn_cast<ArraySubscriptExpr>(expr)) {
      auto *idx = array_subscript_expr->getIdx();
      auto *base = array_subscript_expr->getBase();

      auto res = BuildExpr(idx, {.depth = ctx.depth + 1});
      os << llvm::formatv("{0}[{1}]",
                          Lexer::getSourceText(CharSourceRange::getTokenRange(base->getSourceRange()),
                                               data.Ctx.getSourceManager(), data.Ctx.getLangOpts()),
                          res.final_expr);
      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      // writes are handled by the assignment operator
      if (IsRead(member_expr)) {
        if (auto *field_decl = llvm::dyn_cast<clang::FieldDecl>(member_expr->getMemberDecl())) {
          if (field_decl->isBitField()) {
            const CodeGen::CGBitFieldInfo &info =
                data.code_gen_module.getTypes().getCGRecordLayout(field_decl->getParent()).getBitFieldInfo(field_decl);
            QualType ft = member_expr->getType();
            const auto use_volatile = ft.isVolatileQualified() && IsAAPCS(data.code_gen_module.getTypes().getTarget());
            // stupid AAPCS ABI requires volatile bitfields to be loaded/stored along with their whole container...
            // note that the FIRST volatile bitfield will always have info.VolatileStorageSize == 0 so we omit the
            // check here however, in BuildLoadOfBitFieldLValue, we check for info.VolatileStorageSize != 0 to
            // determine if we should use the volatile path

            if (use_volatile) {
              auto res = BuildLoadOfBitFieldLValue(member_expr, {.depth = ctx.depth + 1});
              os << res.final_expr;
              pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
            } else {
              // otherwise fallback to helper functions

              // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
              BuiltExpr base_built_expr = BuildExpr(member_expr->getBase(), {.depth = ctx.depth + 1});
              llvm::SmallVector<std::string, 4> pre_stmts;
              std::string base_addr = member_expr->isArrow() ? base_built_expr.final_expr
                                                             : llvm::formatv("(&{0})", base_built_expr.final_expr);
              uint64_t start_bit = ((uint64_t)(info.StorageOffset.getQuantity()) * 8) + info.Offset;
              uint64_t end_bit = start_bit + info.Size; // info.Size = bitfield width in bits
              os << llvm::formatv("__c2pnk_get_bitfield_{0}((const uint8_t *){1}, {2}, {3})",
                                  info.IsSigned ? "i64" : "u64", base_addr, start_bit, end_bit);
              pre_stmts.insert(pre_stmts.end(), base_built_expr.pre_stmts.begin(), base_built_expr.pre_stmts.end());
            }

            goto build_expr_end;
          }
        }
      }
      goto build_expr_else;
    } else {
    build_expr_else:
      os << Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), data.Ctx.getSourceManager(),
                                 data.Ctx.getLangOpts());
    }

  build_expr_end:
    os.flush();
    return {.pre_stmts = std::move(pre_stmts),
            .final_expr = std::move(replacement_text),
            .final_expr_type = final_expr_type};
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    auto &sm = data.Ctx.getSourceManager();
    if (declStmt == nullptr || sm.isInSystemHeader(sm.getSpellingLoc(declStmt->getBeginLoc()))) {
      return true;
    }

    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);

    for (auto *decl : declStmt->decls()) {
      if (auto *var_decl = dyn_cast<VarDecl>(decl)) {
        auto *init_expr = var_decl->getInit();
        if (init_expr == nullptr) {
          continue;
        }

        auto res = BuildExpr(init_expr->IgnoreParenImpCasts(), {.depth = 0});
        os << llvm::join(res.pre_stmts | std::views::reverse, "\n");
        os << llvm::formatv("{0} = {1};", PrintType(var_decl->getType(), var_decl->getName()), res.final_expr);
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
    if (stmt == nullptr || sm.isInSystemHeader(sm.getSpellingLoc(stmt->getBeginLoc()))) {
      return true;
    }
    if (auto *expr = dyn_cast<Expr>(stmt)) {
      expr = expr->IgnoreParenImpCasts();

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      auto res = BuildExpr(expr, {.depth = 0});
      os << llvm::join(res.pre_stmts | std::views::reverse, "\n");
      // for some reason we don't need a ; after this...
      os << llvm::formatv("{0}", res.final_expr);
      os.flush();

      if (!replacement_text.empty()) {
        data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                       CharSourceRange::getTokenRange(stmt->getSourceRange()), replacement_text,
                                       data.Ctx.getLangOpts());
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
  CodeGenOptions code_gen_options;
  llvm::LLVMContext llvm_ctx;
  auto layout_probe = std::make_unique<llvm::Module>("layout_probe", llvm_ctx);
  CodeGen::CodeGenModule code_gen_module(Ctx, ci.getVirtualFileSystemPtr(), ci.getHeaderSearchOpts(),
                                         ci.getPreprocessorOpts(), code_gen_options, *layout_probe,
                                         ci.getDiagnostics());

  llvm::SmallVector<Replacement, 64> replacements;
  replacements.emplace_back(Ctx.getSourceManager(),
                            Ctx.getSourceManager().getLocForStartOfFile(Ctx.getSourceManager().getMainFileID()), 0,
                            NON_VOLATILE_BITFIELD_HELPERS);
  WorkerData data{.Ctx = Ctx, .code_gen_module = code_gen_module, .pa_ctx = pa_ctx, .replacements = replacements};
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

  pa_ctx.failure_mode = FailureMode::Success;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_lower_bitfield_ops
