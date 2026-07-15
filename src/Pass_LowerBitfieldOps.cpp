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
#include <vector>

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
  std::vector<Replacement> &replacements;
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
  std::vector<Replacement> replacements;
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
struct WorkerData {
  ASTContext &Ctx;
  CodeGen::CodeGenModule &code_gen_module;
  PipelineActionCtx &pa_ctx;
  std::vector<Replacement> &replacements;
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
    unsigned width = bit_width <= 8 ? 8 : bit_width <= 16 ? 16 : bit_width <= 32 ? 32 : 64;
    return llvm::formatv("{0}{1}_t", prefix, width).str();
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
    llvm::SmallVector<std::string, 4> post_stmts;
    std::string final_expr;
    QualType final_expr_type;
  };

  struct BuiltExprCtx {
    size_t depth = 0;
  };

  // Equivalent to
  // https://clang.llvm.org/doxygen/classclang_1_1CodeGen_1_1CodeGenFunction.html#abce29203390acfa7bfcda7d0c9101629
  auto BuildLoadOfBitFieldLValue(const MemberExpr *member_expr, const BuiltExprCtx &ctx) -> BuiltExpr {
    const FieldDecl *fd = cast<FieldDecl>(member_expr->getMemberDecl());
    assert(fd->isBitField());

    const CodeGen::CGBitFieldInfo &info =
        data.code_gen_module.getTypes().getCGRecordLayout(fd->getParent()).getBitFieldInfo(fd);

    QualType ft = member_expr->getType();

    // Build the base object subexpression (e.g. "s" for s.field, or the pointer expression for p->field)
    BuiltExpr base_built_expr = BuildExpr(member_expr->getBase(), {.depth = ctx.depth + 1});

    llvm::SmallVector<std::string, 4> pre_stmts;
    // pre_stmts.insert(pre_stmts.end(), base_built_expr.pre_stmts.begin(), base_built_expr.pre_stmts.end());

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
      unsigned high_bits = storage_size - offset - info.Size;
      std::string signed_type_name = GetIntTypeName(storage_size, true);

      // Reinterpret as signed before shifting, so ">>" is an arithmetic shift
      // (mirrors CreateShl/CreateAShr operating on a signed value).
      std::string signed_temp = GetTempVarName("bf_signed");
      pre_stmts.push_back(llvm::formatv("{0} {1} = ({0}){2};", signed_type_name, signed_temp, current_temp));
      current_temp = signed_temp;

      // Val = Builder.CreateShl(Val, HighBits, "bf.shl");
      if (high_bits != 0U) {
        std::string shl_temp = GetTempVarName("bf_shl");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} << {3};", signed_type_name, shl_temp, current_temp, high_bits));
        current_temp = shl_temp;
      }
      // Val = Builder.CreateAShr(Val, Offset + HighBits, "bf.ashr");
      if (offset + high_bits != 0U) {
        std::string ashr_temp = GetTempVarName("bf_ashr");
        pre_stmts.push_back(
            llvm::formatv("{0} {1} = {2} >> {3};", signed_type_name, ashr_temp, current_temp, offset + high_bits));
        current_temp = ashr_temp;
      }
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
    return {.pre_stmts = std::move(final_pre_stmts),
            .post_stmts = std::move(base_built_expr.post_stmts),
            .final_expr = cast_temp,
            .final_expr_type = ft};
  }

  auto BuildExpr(Expr *expr, const BuiltExprCtx ctx) -> BuiltExpr {
    expr = expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 4> pre_stmts;
    llvm::SmallVector<std::string, 4> post_stmts;
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
        auto rhs_res = BuildExpr(rhs, {.depth = ctx.depth + 1});
        auto lhs_res = BuildExpr(lhs, {.depth = ctx.depth + 1});

        post_stmts.insert(post_stmts.end(), rhs_res.post_stmts.begin(), rhs_res.post_stmts.end());
        post_stmts.insert(post_stmts.end(), lhs_res.post_stmts.begin(), lhs_res.post_stmts.end());

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

      post_stmts.insert(post_stmts.end(), res.post_stmts.begin(), res.post_stmts.end());

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
        post_stmts.insert(post_stmts.end(), built_expr.post_stmts.begin(), built_expr.post_stmts.end());
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *array_subscript_expr = dyn_cast<ArraySubscriptExpr>(expr)) {
      auto *idx = array_subscript_expr->getIdx();
      auto *base = array_subscript_expr->getBase();

      auto res = BuildExpr(idx, {.depth = ctx.depth + 1});
      post_stmts.insert(post_stmts.end(), res.post_stmts.begin(), res.post_stmts.end());
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
            auto res = BuildLoadOfBitFieldLValue(member_expr, {.depth = ctx.depth + 1});
            post_stmts.insert(post_stmts.end(), res.post_stmts.begin(), res.post_stmts.end());
            pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
            os << res.final_expr;
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
            .post_stmts = std::move(post_stmts),
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
        os << llvm::join(res.post_stmts | std::views::reverse, "\n");
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
      os << llvm::join(res.post_stmts | std::views::reverse, "\n");
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

  std::vector<Replacement> replacements;
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
