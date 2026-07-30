#include "Pass_TransformLogicalExpressions.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
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
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatAdapters.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;
using namespace clang::transformer;
using namespace clang::ast_matchers;

// reduce all logical expressions and expressions with side effects to temporary variables
namespace pancake::pass_hoist_condition_expressions {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all OUTER statements first
  static auto shouldTraversePostOrder() -> bool { return false; }

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.pa_ctx.major_pass_number, data.pa_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto VisitIfStmt(IfStmt *if_stmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (if_stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(if_stmt->getBeginLoc()))) {
      return true;
    }

    const auto *cond = if_stmt->getCond()->IgnoreParenImpCasts();
    if (!cond->HasSideEffects(data.Ctx)) {
      return true;
    }

    auto tmp_var_name = GetTempVarName("If");
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    auto cond_source_text = GetSourceText(cond, data.Ctx);
    if (auto error = cond_source_text.takeError()) {
      data.error = CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nFailed to get source text for IfStmt condition: {1}",
          cond->getExprLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(error)))));
      return false;
    }
    os << llvm::formatv("{0} {1} = {2};\n", GetWordTypeStr(data.Ctx), tmp_var_name, *cond_source_text);
    os << llvm::formatv("if ({0}) ", tmp_var_name);

    // we just want to replace the "if (COND)" part
    os.flush();
    data.replacements.emplace_back(
        data.Ctx.getSourceManager(),
        CharSourceRange::getTokenRange(SourceRange(
            if_stmt->getIfLoc(),
            Lexer::getLocForEndOfToken(cond->getEndLoc(), 0, data.Ctx.getSourceManager(), data.Ctx.getLangOpts()))),
        replacement_text, data.Ctx.getLangOpts());

    return true;
  }

  auto VisitReturnStmt(ReturnStmt *return_stmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (return_stmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(return_stmt->getBeginLoc()))) {
      return true;
    }

    const auto *ret_expr = return_stmt->getRetValue();
    if (ret_expr == nullptr) {
      return true;
    }

    const auto *cond = ret_expr->IgnoreParenImpCasts();
    if (!cond->HasSideEffects(data.Ctx)) {
      return true;
    }

    auto tmp_var_name = GetTempVarName("Return");
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    auto cond_source_text = GetSourceText(cond, data.Ctx);
    if (auto error = cond_source_text.takeError()) {
      data.error = CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nFailed to get source text for ReturnStmt expression: {1}",
          cond->getExprLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(error)))));
      return false;
    }
    os << llvm::formatv("{0} {1} = {2};\n", GetWordTypeStr(data.Ctx), tmp_var_name, *cond_source_text);
    os << llvm::formatv("return {0}", tmp_var_name);

    // we want to replace the "return EXPR;" part
    os.flush();
    data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                   CharSourceRange::getTokenRange(return_stmt->getSourceRange()), replacement_text,
                                   data.Ctx.getLangOpts());

    return true;
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .pa_ctx = ps_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  int errors = 0;
  for (const auto &r : replacements) {
    if (auto err = ps_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      errors++;
    }
  }

  if (errors == 0 && replacements.empty()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    return;
  }
  if (errors > 0) {
    PrintLogBegin(llvm::outs(), ps_ctx);
    llvm::outs() << llvm::formatv("Couldn't add {0} replacement{1}\n", errors, errors != 1 ? "s" : "");
  }
  ps_ctx.whats_next = WhatsNext::RepeatPass;
}
} // namespace pancake::pass_hoist_condition_expressions

namespace pancake::pass_rewrite_array_indexing {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(
      arraySubscriptExpr(isExpansionInMainFile(), hasBase(expr().bind("base")), hasIndex(expr().bind("index")))
          .bind("array_subscript"),
      changeTo(node("array_subscript"), cat("(*(", node("base"), " + (", node("index"), ")))")));
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
} // namespace pancake::pass_rewrite_array_indexing

namespace pancake::pass_rewrite_struct_stabs {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(memberExpr(isExpansionInMainFile(), isArrow(), hasObjectExpression(expr().bind("base")),
                             member(fieldDecl().bind("field")))
                      .bind("member"),
                  changeTo(node("member"), cat("(*", node("base"), ").", name("field"))));
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
} // namespace pancake::pass_rewrite_struct_stabs

namespace pancake::pass_lower_nested_expressions {
auto GetUsage(const ASTContext &ctx, const Expr *expr) -> Expected<Usage> {
  expr = expr->IgnoreParenImpCasts();

  if (const auto *_ = dyn_cast<DeclRefExpr>(expr)) {
    return Usage::Place;
  }
  if (const auto *_ = dyn_cast<IntegerLiteral>(expr)) {
    return Usage::Place;
  }
  if (const auto *_ = dyn_cast<FloatingLiteral>(expr)) {
    return Usage::Place;
  }
  if (const auto *_ = dyn_cast<CharacterLiteral>(expr)) {
    return Usage::Place;
  }
  if (const auto *_ = dyn_cast<StringLiteral>(expr)) {
    return Usage::Place;
  }
  if (const auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
    return GetUsage(ctx, c_style_cast_expr->getSubExpr());
  }
  if (const auto *_ = dyn_cast<CompoundLiteralExpr>(expr)) {
    return Usage::Value;
  }
  if (const auto *_ = dyn_cast<InitListExpr>(expr)) {
    return Usage::Value;
  }
  if (const auto *_ = dyn_cast<StmtExpr>(expr)) {
    return Usage::Value;
  }
  // CompoundAssignOperator is a specialization of BinaryOperator
  if (const auto *_ = dyn_cast<CompoundAssignOperator>(expr)) {
    return Usage::Value;
  }
  if (const auto *_ = dyn_cast<ConditionalOperator>(expr)) {
    return Usage::Value;
  }
  if (const auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
    switch (binary_operator->getOpcode()) {
    case BO_LAnd:
      [[fallthrough]];
    case BO_LOr:
      [[fallthrough]];
    case BO_Assign:
      [[fallthrough]];
    case BO_Comma:
      [[fallthrough]];
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
      return Usage::Value;
    }

    default: {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nUnhandled binary operator: {1}",
                                  binary_operator->getExprLoc().printToString(ctx.getSourceManager()),
                                  BinaryOperator::getOpcodeStr(binary_operator->getOpcode()))));
      break;
    }
    }
  } else if (const auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
    switch (unary_operator->getOpcode()) {
    case UO_PostInc:
      [[fallthrough]];
    case UO_PostDec: {
      return Usage::Value;
    }

    case UO_PreInc:
      [[fallthrough]];
    case UO_PreDec: {
      return Usage::Value;
    }

    case UO_Deref: {
      return Usage::Place;
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
      return Usage::Place;
    }

    default: {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nUnhandled unary operator: {1}",
                                  unary_operator->getExprLoc().printToString(ctx.getSourceManager()),
                                  UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str())));
      break;
    }
    }
  } else if (const auto *_ = dyn_cast<CallExpr>(expr)) {
    return Usage::Value;
  } else {
    return Usage::Place;
  }
}

namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.ps_ctx.major_pass_number, data.ps_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto PrintType(const QualType ty, const llvm::StringRef var_name) const -> std::string {
    std::string s;
    llvm::raw_string_ostream os(s);
    ty.print(os, data.Ctx.getPrintingPolicy(), var_name);
    return os.str();
  }

  static auto CompoundAssignOpToOp(BinaryOperatorKind compound_assign_op) -> Expected<BinaryOperatorKind> {
    switch (compound_assign_op) {
    case BO_MulAssign:
      return BO_Mul;
    case BO_DivAssign:
      return BO_Div;
    case BO_RemAssign:
      return BO_Rem;
    case BO_AddAssign:
      return BO_Add;
    case BO_SubAssign:
      return BO_Sub;
    case BO_ShlAssign:
      return BO_Shl;
    case BO_ShrAssign:
      return BO_Shr;
    case BO_AndAssign:
      return BO_And;
    case BO_XorAssign:
      return BO_Xor;
    case BO_OrAssign:
      return BO_Or;
    default:
      return CreateRuntimeError(std::move(llvm::formatv("Unhandled compound assignment operator: {0}",
                                                        BinaryOperator::getOpcodeStr(compound_assign_op).str())));
    }
  }

  auto BuildExpr(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    auto *expr = ctx.expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const QualType final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      if (auto err = PrintSourceText(os, decl_ref_expr, data.Ctx)) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to print source text for DeclRefExpr: {1}",
                                    decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(err)))));
      }
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      if (auto err = PrintSourceText(os, integer_literal, data.Ctx)) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to print source text for IntegerLiteral: {1}",
                                    integer_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(err)))));
      }
    } else if (auto *floating_literal = dyn_cast<FloatingLiteral>(expr)) {
      if (auto err = PrintSourceText(os, floating_literal, data.Ctx)) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to print source text for FloatingLiteral: {1}",
                                    floating_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(err)))));
      }
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      if (auto err = PrintSourceText(os, character_literal, data.Ctx)) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to print source text for CharacterLiteral: {1}",
                                    character_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(err)))));
      }
    } else if (auto *string_literal = dyn_cast<StringLiteral>(expr)) {
      if (ctx.string_literal_usage_kind.has_value()) {
        auto usage_kind = ctx.string_literal_usage_kind.value();
        std::string expanded;
        llvm::raw_string_ostream os2(expanded);
        os2 << "{ ";
        for (auto c : string_literal->getString()) {
          os2 << "'" << c << "', ";
        }
        os2 << "'\\0' }";
        os2.flush();

        switch (usage_kind) {
        case BuildExprCtx::StringLiteralUsageKind::AsArray: {
          // expand to { 'char1', 'char2' ... 'charN', '\0' } and repeat the pass
          // note there's an edge case here where we can have char a[3] = "abc"; and there is no null terminator but I
          // can't be bothered dealing with this edge case
          os << expanded;
          break;
        }
        case BuildExprCtx::StringLiteralUsageKind::AsPointer: {
          // create new char array variable and return the pointer to the zero-th element of the array,
          //
          // e.g. "char __c2pnk_str_0[] = { 'char1', 'char2', ..., 'charN', '\0' };" and return "__c2pnk_str_0"
          std::string tmp_var_name = GetTempVarName("StringLiteral");
          pre_stmts.emplace_back(
              llvm::formatv("{0} = {1};", PrintType(string_literal->getType(), tmp_var_name), expanded));
          os << tmp_var_name;
          break;
        }
        default: {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nUnhandled StringLiteralUsageKind: {1}",
                                      string_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      std::to_underlying(usage_kind))));
        }
        }
        data.ps_ctx.whats_next = WhatsNext::RepeatPass;
      } else {
        if (auto err = PrintSourceText(os, string_literal, data.Ctx)) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to print source text for StringLiteral: {1}",
                                      string_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(err)))));
        }
      }
    } else if (auto *implicit_value_init_expr = dyn_cast<ImplicitValueInitExpr>(expr)) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nImplicitValueInitExpr should not be present at this stage!",
                                  implicit_value_init_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
    } else if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto usage = GetUsage(data.Ctx, sub_expr);
      if (auto error = usage.takeError()) {
        return std::move(error);
      }
      auto res = BuildExpr(
          BuildExprCtx(sub_expr, *usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      os << "(";
      c_style_cast_expr->getTypeAsWritten().print(os, data.Ctx.getPrintingPolicy());
      os << ")(";
      os << res->final_expr;
      os << ")";

      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else if (auto *init_list_expr = dyn_cast<InitListExpr>(expr)) {
      if (ctx.usage_kind == Usage::Place || ctx.usage_kind == Usage::Effect) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nInitListExpr cannot be used as a place or effect expression",
                                    init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      if (!ctx.assigned_to.has_value()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nInitListExpr must have an assigned_to value",
                                    init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      const auto &[_var_name, _var_type] = ctx.assigned_to.value();
      auto var_name = _var_name.get();
      auto var_type = _var_type.get();

      if (var_type->isArrayType()) {
        if (!var_type->isConstantArrayType()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nInitListExpr must be assigned to a constant array type",
                                      init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }
        // e.g int arr[3][4][5] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
        auto element_type = data.Ctx.getAsConstantArrayType(var_type)->getElementType();

        llvm::SmallVector<std::string, 16> tmp_stmts; // this is in real order. it will need to be reversed later
        if (ctx.deref_force_extract) {
          auto tmp_var_type = data.Ctx.getPointerType(
              element_type); // we want to assign to a pointer to the element type, not the element type itself

          // must extract var to tmp var first, then assign to array
          const std::string tmp_var_name = GetTempVarName("InitList");
          tmp_stmts.emplace_back(llvm::formatv("{0} = {1};", PrintType(tmp_var_type, tmp_var_name), var_name));
          var_name = tmp_var_name;
        }

        for (unsigned i = 0; i < init_list_expr->getNumInits(); ++i) {
          auto *init_expr = init_list_expr->getInit(i);

          if (auto *_ = dyn_cast<ImplicitValueInitExpr>(init_expr)) {
            continue;
          }

          const std::string element_var_name = llvm::formatv("*({0} + {1}UL)", var_name, i);
          auto usage = GetUsage(data.Ctx, init_expr);
          if (auto error = usage.takeError()) {
            return std::move(error);
          }
          auto res = BuildExpr(BuildExprCtx(init_expr, *usage, true, std::make_pair(element_var_name, element_type),
                                            ctx.string_literal_usage_kind));
          if (auto error = res.takeError()) {
            return std::move(error);
          }

          // insert in REAL order
          tmp_stmts.insert(tmp_stmts.end(), res->pre_stmts.rbegin(), res->pre_stmts.rend());

          // if there are nested init expr lists for arrays, then final_expr will be empty!
          if (!res->final_expr.empty()) {
            tmp_stmts.emplace_back(llvm::formatv("{0} = {1};", element_var_name, res->final_expr));
          }
        }

        pre_stmts.assign(tmp_stmts.rbegin(), tmp_stmts.rend()); // reverse the order of the statements
      } else if (var_type->isUnionType()) {
        auto *field_decl = init_list_expr->getInitializedFieldInUnion();
        if (field_decl != nullptr) {
          if (init_list_expr->getNumInits() != 1) {
            return CreateRuntimeError(
                std::move(llvm::formatv("    at {0}\nInitListExpr for union must have exactly one initializer",
                                        init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
          }
          auto *init_expr = init_list_expr->getInit(0);

          if (auto *_ = dyn_cast<ImplicitValueInitExpr>(init_expr)) {
            goto build_expr_end; // nothing to preserve
          }

          if (field_decl->isAnonymousStructOrUnion()) {
            auto *sub_init_expr_list = dyn_cast<InitListExpr>(init_expr);
            if (sub_init_expr_list == nullptr) {
              return CreateRuntimeError(
                  std::move(llvm::formatv("    at {0}\nInitListExpr for anonymous struct or union must have an "
                                          "InitListExpr as its initializer",
                                          init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
            }
          } else {
            var_name = llvm::formatv("({0}).{1}", var_name, field_decl->getNameAsString());
          }

          // edge case for string literals initializing char arrays, we need to generate a series of assignments instead
          // of a single assignment. need to do it in reverse order here
          if (auto *string_literal = dyn_cast<StringLiteral>(init_expr)) {
            if (field_decl->getType()->isArrayType()) {
              pre_stmts.emplace_back(llvm::formatv("*({0} + {1}UL) = '\\0';", var_name, string_literal->getLength()));
              for (auto [i, c] : string_literal->getString() | std::views::enumerate | std::views::reverse) {
                pre_stmts.emplace_back(llvm::formatv("*({0} + {1}UL) = '{2}';", var_name, i, c));
              }
              goto build_expr_end;
            }
          }

          auto usage = GetUsage(data.Ctx, init_expr);
          if (auto error = usage.takeError()) {
            return std::move(error);
          }
          auto res =
              BuildExpr(BuildExprCtx(init_expr, *usage, ctx.deref_force_extract,
                                     std::make_pair(var_name, field_decl->getType()), ctx.string_literal_usage_kind));
          if (auto error = res.takeError()) {
            return std::move(error);
          }

          if (!res->final_expr.empty()) {
            pre_stmts.emplace_back(llvm::formatv("{0} = {1};", var_name, res->final_expr));
          }

          pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        } else {
          // C23 §6.7.11p11
          // if it is a union, the first named member is initialized (recursively) according to these rules, and any
          // padding is initialized to zero bits.
          // note CRITICALLY that padding != subsequent members!!!!!!!!
          // but historically the compiler implementations have just memset the whole thing to 0
          // init_list_stmts.emplace_back(
          //     llvm::formatv("memset(&{0}, 0, sizeof({1}));", var_name, PrintType(var_type, "")));
        }
      } else if (var_type->isStructureType()) {
        auto *record_decl = var_type->getAsRecordDecl();
        llvm::SmallVector<std::string, 16> tmp_stmts; // this is in real order. it will need to be reversed later

        auto field_decl_iter = record_decl->field_begin();
        for (unsigned i = 0; i < init_list_expr->getNumInits(); ++field_decl_iter) {
          auto *field_decl = *field_decl_iter;

          if (field_decl->isUnnamedBitField()) {
            // very nasty potential bug here: don't increment i!
            continue; // skip unnamed bitfields
          }

          auto *init_expr = init_list_expr->getInit(i);
          if (auto *_ = dyn_cast<ImplicitValueInitExpr>(init_expr)) {
            /*
  struct A {
    int x;
    int y;
    struct B {
      int z;
      int w;
    };
  };

  struct A a = {.y = 2};

  will produce

   |-DeclStmt 0x5c835e0af9a8 <line:25:3, col:24>
    | `-VarDecl 0x5c835e0af7d8 <col:3, col:23> col:12 a 'struct A' cinit
    |   `-InitListExpr 0x5c835e0af948 <col:16, col:23> 'struct A'
    |     |-ImplicitValueInitExpr 0x5c835e0af998 <<invalid sloc>> 'int'
    |     `-IntegerLiteral 0x5c835e0af840 <col:22> 'int' 2

  Notice how since A.x is declared first but not specified with a designated initializer, it has been replaced with an
  ImplicitValueInitExpr. That's why we continue here instead of break.
            */
            ++i;
            continue;
          }
          init_expr = init_expr->IgnoreParenImpCasts();

          auto i_var_name = var_name;
          if (field_decl->isAnonymousStructOrUnion()) {
            auto *sub_init_expr_list = dyn_cast<InitListExpr>(init_expr);
            if (sub_init_expr_list == nullptr) {
              return CreateRuntimeError(
                  std::move(llvm::formatv("    at {0}\nInitListExpr for anonymous struct or union must have an "
                                          "InitListExpr as its initializer",
                                          init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
            }
          } else {
            i_var_name = llvm::formatv("({0}).{1}", i_var_name, field_decl->getNameAsString());
          }

          auto usage = GetUsage(data.Ctx, init_expr);
          if (auto error = usage.takeError()) {
            return std::move(error);
          }

          // edge case for string literals initializing char arrays, we need to generate a series of assignments instead
          // of a single assignment
          if (auto *string_literal = dyn_cast<StringLiteral>(init_expr)) {
            if (field_decl->getType()->isArrayType()) {
              for (auto [i, c] : string_literal->getString() | std::views::enumerate) {
                tmp_stmts.emplace_back(llvm::formatv("*({0} + {1}UL) = '{2}';", i_var_name, i, c));
              }
              tmp_stmts.emplace_back(llvm::formatv("*({0} + {1}UL) = '\\0';", i_var_name, string_literal->getLength()));
              i++;
              continue;
            }
          }

          auto res =
              BuildExpr(BuildExprCtx(init_expr, *usage, ctx.deref_force_extract,
                                     std::make_pair(i_var_name, field_decl->getType()), ctx.string_literal_usage_kind));
          if (auto error = res.takeError()) {
            return std::move(error);
          }

          // insert in REAL order
          tmp_stmts.insert(tmp_stmts.end(), res->pre_stmts.rbegin(), res->pre_stmts.rend());

          // if there are nested init expr lists for arrays, then final_expr will be empty!
          if (!res->final_expr.empty()) {
            tmp_stmts.emplace_back(llvm::formatv("{0} = {1};", i_var_name, res->final_expr));
          }

          i++;
        }

        pre_stmts.assign(tmp_stmts.rbegin(), tmp_stmts.rend()); // reverse the order of the statements
      } else {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nInitListExpr must be assigned to an array, union, or struct type",
                                    init_list_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      // don't write anything to os
    } else if (auto *compound_literal = dyn_cast<CompoundLiteralExpr>(expr)) {
      // must extract to temp var
      std::string tmp_var_name = GetTempVarName("CompoundLiteral");
      auto compound_literal_initializer =
          GetSourceText(compound_literal->getInitializer()->IgnoreParenImpCasts(), data.Ctx);
      if (auto error = compound_literal_initializer.takeError()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nFailed to get source text for CompoundLiteralExpr initializer: {1}",
                                    compound_literal->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    llvm::fmt_consume(std::move(error)))));
      }
      pre_stmts.push_back(llvm::formatv(
          /*
          0 = tmp var for compound literal
          1 = tmp var name (no type!)
          2 = compound literal initializer expression (always exists for a compound literal)
          */
          R"({0};
{1} = {2};)",
          PrintType(compound_literal->getType(), tmp_var_name), tmp_var_name, *compound_literal_initializer));
      os << tmp_var_name;
      // we need to rerun this pass to process the compound literal initializer expression
      data.ps_ctx.whats_next = WhatsNext::RepeatPass;
    } else if (auto *statement_expr = dyn_cast<StmtExpr>(expr)) {
      // StmtExpr is a GNU extension that allows a block of statements to be used as an expression
      // The value of the expression is the value of the last statement in the block
      auto *compound_stmt = statement_expr->getSubStmt();
      auto *last = compound_stmt->body_back();
      auto *last_stmt_as_expr = dyn_cast<Expr>(last);
      if (last_stmt_as_expr == nullptr) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nLast statement in StmtExpr must be an expression",
                                    statement_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      last_stmt_as_expr = last_stmt_as_expr->IgnoreParenImpCasts();

      if (ctx.usage_kind == Usage::Place) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nStmtExpr cannot be used as a place expression",
                                    statement_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      std::string pre_block;
      llvm::raw_string_ostream os2(pre_block);

      // if the usage kind is effect, we don't need a tmp var at all
      if (ctx.usage_kind == Usage::Effect) {
        os2 << "{\n";
        for (auto *stmt : compound_stmt->body()) {
          if (auto err = PrintSourceText(os2, stmt, data.Ctx)) {
            return CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to print source text for statement in StmtExpr: {1}",
                stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
          }
          if (StmtNeedsSemi(stmt)) {
            os2 << ";";
          }
          os2 << "\n";
          // we need to rerun this pass to process the statements in the block
          data.ps_ctx.whats_next = WhatsNext::RepeatPass;
        }
        os2 << "}\n";
        // don't add anything to os
      } else {
        std::string tmp_var_name = GetTempVarName("GNUStmtExprResult");
        if (!last_stmt_as_expr->getType()->isVoidType()) {
          os2 << llvm::formatv("{0};\n", PrintType(last_stmt_as_expr->getType(), tmp_var_name));
        }
        os2 << "{\n";
        for (auto *stmt : compound_stmt->body()) {
          if (stmt == last) {
            auto usage = GetUsage(data.Ctx, last_stmt_as_expr);
            if (auto error = usage.takeError()) {
              return std::move(error);
            }
            auto last_expr_built_expr = BuildExpr(BuildExprCtx(last_stmt_as_expr, *usage, ctx.deref_force_extract,
                                                               ctx.assigned_to, ctx.string_literal_usage_kind));
            if (auto error = last_expr_built_expr.takeError()) {
              return std::move(error);
            }

            os2 << llvm::join(last_expr_built_expr->pre_stmts | std::views::reverse, "\n");

            // assign the result of the last expression to the temporary variable
            if (!last_stmt_as_expr->getType()->isVoidType()) {
              os2 << llvm::formatv("\n{0} = {1};", tmp_var_name, last_expr_built_expr->final_expr);
            } else {
              os2 << llvm::formatv("\n(void){0};", last_expr_built_expr->final_expr);
            }
          } else {
            if (auto err = PrintSourceText(os2, stmt, data.Ctx)) {
              return CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nFailed to print source text for statement in StmtExpr: {1}",
                  stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
            }
            if (StmtNeedsSemi(stmt)) {
              os2 << ";";
            }
            os2 << "\n";
            // we need to rerun this pass to process the statements in the block
            data.ps_ctx.whats_next = WhatsNext::RepeatPass;
          }
        }
        os2 << "}\n";
        os << tmp_var_name;
      }

      os2.flush();
      pre_stmts.push_back(pre_block);
    }
    // CompoundAssignOperator is a specialization of BinaryOperator
    else if (auto *compound_assign_operator = dyn_cast<CompoundAssignOperator>(expr)) {
      auto *lhs = compound_assign_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = compound_assign_operator->getRHS()->IgnoreParenImpCasts();

      // technically we need to compute both the place and value of the LHS, but syntactically at least,
      // every valid place is also a valid value (not the other way around though!)
      // note this is only true since we are working with a "description" of the place, not a pointer!
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
        return CreateRuntimeError(std::move(
            llvm::formatv("    at {0}\nCompound assignment operator cannot be used as a place expression",
                          compound_assign_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      auto compound_assign_op = CompoundAssignOpToOp(compound_assign_operator->getOpcode());
      if (auto error = compound_assign_op.takeError()) {
        return error;
      }
      // don't care about temp var
      if (ctx.usage_kind == Usage::Effect) {
        pre_stmts.push_back(llvm::formatv("{0} = {0} {1} {2};\n", lhs_res->final_expr,
                                          BinaryOperator::getOpcodeStr(*compound_assign_op), rhs_res->final_expr));
      } else {
        std::string tmp_var_name = GetTempVarName("CompoundAssignResult");
        pre_stmts.push_back(llvm::formatv(
                                /*
                                0 = temp var for result of compound assignment
                                1 = lhs final expr
                                2 = binary operator string for compound assignment
                                3 = rhs final expr
                                4 = tmp var name (without type!)
                                */
                                R"({0} = {1} {2} {3};
{1} = {4};)",
                                PrintType(compound_assign_operator->getType(), tmp_var_name), lhs_res->final_expr,
                                BinaryOperator::getOpcodeStr(*compound_assign_op), rhs_res->final_expr, tmp_var_name)
                                .str());
        os << tmp_var_name;
      }
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
    } else if (auto *conditional_operator = dyn_cast<ConditionalOperator>(expr)) {
      auto *cond = conditional_operator->getCond()->IgnoreParenImpCasts();
      auto *lhs = conditional_operator->getTrueExpr()->IgnoreParenImpCasts();
      auto *rhs = conditional_operator->getFalseExpr()->IgnoreParenImpCasts();

      auto cond_usage = GetUsage(data.Ctx, cond);
      if (auto error = cond_usage.takeError()) {
        return std::move(error);
      }
      auto cond_res = BuildExpr(
          BuildExprCtx(cond, *cond_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
      if (auto error = cond_res.takeError()) {
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
      auto rhs_usage = GetUsage(data.Ctx, rhs);
      if (auto error = rhs_usage.takeError()) {
        return std::move(error);
      }
      auto rhs_res = BuildExpr(
          BuildExprCtx(rhs, *rhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }

      std::string tmp_var_name = GetTempVarName("TernaryResult");
      std::string tmp_cond_name = GetTempVarName("TernaryCond");
      const std::string if_cond = llvm::formatv(
          /*
          0 = var for result of ?: (inc type)
          1 = var for result (tmp var name)
          2 = condition bool pre stmts
          3 = condition bool type (GetWordTypeStr)
          4 = condition bool name
          5 = condition bool final expr
          6 = lhs pre stmts
          7 = lhs final expr
          8 = rhs pre stmts
          9 = rhs final expr
          */
          R"({0};
{2}
{3} {4} = {5};
if ({4}) {
  {6}
  {1} = {7};
} else {
  {8}
  {1} = {9};
}
)",
          PrintType(conditional_operator->getType(), tmp_var_name), tmp_var_name,
          llvm::join(cond_res->pre_stmts | std::views::reverse, "\n"), GetWordTypeStr(data.Ctx), tmp_cond_name,
          cond_res->final_expr, llvm::join(lhs_res->pre_stmts | std::views::reverse, "\n"), lhs_res->final_expr,
          llvm::join(rhs_res->pre_stmts | std::views::reverse, "\n"), rhs_res->final_expr);

      pre_stmts.push_back(if_cond);
      os << tmp_var_name;
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
        // LAnd and LOr are handled specially because they short circuit
      case BO_LAnd: {
        auto lhs_usage = GetUsage(data.Ctx, lhs);
        if (auto error = lhs_usage.takeError()) {
          return error;
        }
        auto lhs_res = BuildExpr(
            BuildExprCtx(lhs, *lhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = lhs_res.takeError()) {
          return error;
        }
        auto rhs_usage = GetUsage(data.Ctx, rhs);
        if (auto error = rhs_usage.takeError()) {
          return error;
        }
        auto rhs_res = BuildExpr(
            BuildExprCtx(rhs, *rhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = rhs_res.takeError()) {
          return error;
        }
        std::string tmp_var_name = GetTempVarName("LAnd");
        auto expr_source_text = GetSourceText(expr, data.Ctx);
        if (auto error = expr_source_text.takeError()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to get source text for logical && expression: {1}",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
        std::string const if_cond = llvm::formatv(
            /*
            0 = GetSourceText
            1 = lhs pre stmts
            2 = GetWordTypeStr
            3 = tmp var for result of &&
            4 = lhs final expr
            5 = rhs pre stmts (only evaluated if lhs is true)
            6 = rhs final expr (only evaluated if lhs is true)
            7 = GetWordTypeStr
            */
            R"(/* c2pancake: transformed logical && */
/* original expr: {0} */
{1}
{2} {3} = 0UL;
if (!({4})) {
  {3} = 0UL;
} else {
  {5}
  {3} = 0UL != ({6}); 
})",
            *expr_source_text, llvm::join(lhs_res->pre_stmts | std::views::reverse, "\n"), GetWordTypeStr(data.Ctx),
            tmp_var_name, lhs_res->final_expr, llvm::join(rhs_res->pre_stmts | std::views::reverse, "\n"),
            rhs_res->final_expr);

        pre_stmts.push_back(if_cond);
        os << tmp_var_name;
        break;
      }
      case BO_LOr: {
        auto lhs_usage = GetUsage(data.Ctx, lhs);
        if (auto error = lhs_usage.takeError()) {
          return std::move(error);
        }
        auto lhs_res = BuildExpr(
            BuildExprCtx(lhs, *lhs_usage, ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
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
        std::string tmp_var_name = GetTempVarName("LOr");
        auto expr_source_text = GetSourceText(expr, data.Ctx);
        if (auto error = expr_source_text.takeError()) {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nFailed to get source text for logical || expression: {1}",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      llvm::fmt_consume(std::move(error)))));
        }
        const std::string if_cond = llvm::formatv(
            /*
            0 = GetSourceText
            1 = lhs pre stmts
            2 = GetWordTypeStr
            3 = tmp var for result of ||
            4 = lhs final expr
            5 = rhs pre stmts (only evaluated if lhs is false)
            6 = rhs final expr (only evaluated if lhs is false)
            */
            R"(/* c2pancake: transformed logical || */
/* original expr: {0} */
{1}
{2} {3} = 0UL;
if ({4}) {
  {3} = 1UL;
} else {
  {5}
  {3} = 0UL != ({6});
})",
            *expr_source_text, llvm::join(lhs_res->pre_stmts | std::views::reverse, "\n"), GetWordTypeStr(data.Ctx),
            tmp_var_name, lhs_res->final_expr, llvm::join(rhs_res->pre_stmts | std::views::reverse, "\n"),
            rhs_res->final_expr);
        pre_stmts.push_back(if_cond);
        os << tmp_var_name;
        break;
      }

      case BO_Assign: {
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
              std::move(llvm::formatv("    at {0}\nAssignment operator cannot be used as a place expression",
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

      case BO_Comma: {
        // LHS is evaluated first, then RHS
        // RHS is returned (only when ctx.usage_kind == Value)

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
        pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());

        os << rhs_res->final_expr;
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
              std::move(llvm::formatv("    at {0}\nBinary operator cannot be used as a place expression",
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
            std::move(llvm::formatv("    at {0}\nUnhandled binary operator: {1}",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    BinaryOperator::getOpcodeStr(binary_operator->getOpcode()))));
        break;
      }
      }
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();

      switch (unary_operator->getOpcode()) {
      case UO_PostInc:
        [[fallthrough]];
      case UO_PostDec: {
        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nPostfix increment/decrement operator cannot be used as a place "
                                      "expression",
                                      unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place, ctx.deref_force_extract, ctx.assigned_to,
                                          ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return error;
        }

        if (ctx.usage_kind == Usage::Effect) {
          pre_stmts.push_back(
              llvm::formatv(
                  /*
                  0 = value of the sub expression
                  1 = the operator (+ or -)
                  */
                  R"({0} = {0} {1} 1UL;)", res->final_expr, unary_operator->getOpcode() == UO_PostInc ? "+" : "-")
                  .str());
        } else {
          std::string const tmp_var_name =
              GetTempVarName(unary_operator->getOpcode() == UO_PostInc ? "PostInc" : "PostDec");
          pre_stmts.push_back(llvm::formatv(
                                  /*
                                  0 = tmp var for result of postfix inc/dec
                                  1 = value of the sub expression
                                  2 = the operator (+ or -)
                                  */
                                  R"({0} = {1};
{1} = {1} {2} 1UL;)",
                                  PrintType(res->final_expr_type, tmp_var_name), res->final_expr,
                                  unary_operator->getOpcode() == UO_PostInc ? "+" : "-")
                                  .str());
          os << tmp_var_name;
        }

        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

      case UO_PreInc:
        [[fallthrough]];
      case UO_PreDec: {
        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(std::move(
              llvm::formatv("    at {0}\nPrefix increment/decrement operator cannot be used as a place expression",
                            unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place, ctx.deref_force_extract, ctx.assigned_to,
                                          ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return error;
        }

        if (ctx.usage_kind == Usage::Effect) {
          pre_stmts.push_back(
              llvm::formatv(
                  /*
                  0 = value of the sub expression
                  1 = the operator (+ or -)
                  */
                  R"({0} = {0} {1} 1UL;)", res->final_expr, unary_operator->getOpcode() == UO_PreInc ? "+" : "-")
                  .str());
        } else {
          std::string const tmp_var_name =
              GetTempVarName(unary_operator->getOpcode() == UO_PreInc ? "PreInc" : "PreDec");
          pre_stmts.push_back(llvm::formatv(
                                  /*
                                  0 = tmp var for result of Prefix inc/dec
                                  1 = value of the sub expression
                                  2 = the operator (+ or -)
                                  */
                                  R"({1} = {1} {2} 1UL;
                                  {0} = {1};)",
                                  PrintType(res->final_expr_type, tmp_var_name), res->final_expr,
                                  unary_operator->getOpcode() == UO_PreInc ? "+" : "-")
                                  .str());
          os << tmp_var_name;
        }

        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        break;
      }

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
        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nUnary operator cannot be used as a place expression",
                                      unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }
        break;
      }
      }
    } else if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        auto res = BuildExpr(BuildExprCtx(arg->IgnoreParenImpCasts(), Usage::Value, ctx.deref_force_extract,
                                          ctx.assigned_to, ctx.string_literal_usage_kind));
        if (auto error = res.takeError()) {
          return error;
        }
        arg_built_exprs.push_back(std::move(*res));
      }
      // TODO
      // it's possible for getDirectCallee to return nullptr, but I don't know what to do in that case...
      // TODO throw error on any function pointers
      os << llvm::formatv("{0}({1})", call_expr->getDirectCallee()->getName().str(),
                          llvm::join(arg_built_exprs | std::views::transform([](const BuiltExpr &e) -> std::string {
                                       return e.final_expr;
                                     }),
                                     ", "));
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }
    } else if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      if (member_expr->isArrow()) {
        llvm::outs() << "member expr location: "
                     << member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()) << "\n";
      }
      if (member_expr->isArrow()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nArrow member access is not allowed here",
                                    member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()))));
      }

      auto res = BuildExpr(BuildExprCtx(member_expr->getBase()->IgnoreParenImpCasts(), Usage::Place,
                                        ctx.deref_force_extract, ctx.assigned_to, ctx.string_literal_usage_kind));
      if (auto error = res.takeError()) {
        return error;
      }
      os << llvm::formatv("({0}).{1}", res->final_expr, member_expr->getMemberNameInfo().getAsString());
      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else {
      if (auto err = PrintSourceText(os, expr, data.Ctx)) {
        return CreateRuntimeError(std::move(llvm::formatv(
            "\n    at {0}\nFailed to print source text for expression: {1}",
            expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(err)))));
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
      if (auto *record_decl = dyn_cast<RecordDecl>(decl)) {
        // check if definition is in the same place
        /*
        Turn instances of
          struct S { int x; } s;
        into
          struct S { int x; };
          struct S a;
        */
        if (record_decl->isThisDeclarationADefinition()) {
          if (auto err = PrintSourceText(os, record_decl, data.Ctx)) {
            data.error = CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nFailed to print source text for RecordDecl: {1}",
                                        record_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                        llvm::fmt_consume(std::move(err)))));
            return false;
          }
          os << ";";
        }
      } else if (auto *var_decl = dyn_cast<VarDecl>(decl)) {
        auto *init_expr = var_decl->getInit();
        if (init_expr != nullptr) {
          // normally we would put Usage::Value here but since every Usage::Place is also a Usage::Value, we can just
          // use Usage::Place to avoid an extra copy of the expression
          init_expr = init_expr->IgnoreParenImpCasts();
          auto usage = GetUsage(data.Ctx, init_expr);
          if (auto error = usage.takeError()) {
            data.error = std::move(error);
            return false;
          }

          std::optional<BuildExprCtx::StringLiteralUsageKind> string_literal_usage_kind = std::nullopt;
          if (auto *_ = dyn_cast<StringLiteral>(init_expr)) {
            auto var_decl_type = var_decl->getType();
            if (const auto *_ = dyn_cast<PointerType>(var_decl_type.getTypePtr())) {
              string_literal_usage_kind = BuildExprCtx::StringLiteralUsageKind::AsPointer;
            } else if (const auto *array_type = dyn_cast<ArrayType>(var_decl_type.getTypePtr())) {
              auto element = array_type->getElementType();
              if (!element->isCharType()) {
                data.error = CreateRuntimeError(std::move(llvm::formatv(
                    "\n    at {0}\nString literal cannot be assigned to non-char array type: {1}",
                    init_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl_type.getAsString())));
                return false;
              }
              string_literal_usage_kind = BuildExprCtx::StringLiteralUsageKind::AsArray;
            }
          }

          auto res = BuildExpr(BuildExprCtx(
              init_expr, *usage, false,
              std::make_optional(std::make_pair(var_decl->getNameAsString(), var_decl->getType().getCanonicalType())),
              string_literal_usage_kind));
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
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  // TODO how does this interact with RunResult??/
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

  // If a GNU statement expression was encountered, we need to rerun this pass
  // The statment expr contains a block of statements which are not recursively traversed by the AST visitor in
  // this pass. By running this pass again, we can ensure that any nested expressions within the statement expression
  // are also transformed.
  if (!ps_ctx.whats_next.has_value()) {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
  }
}
} // namespace pancake::pass_lower_nested_expressions

namespace pancake::pass_simplify_double_negation {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(
      unaryOperator(isExpansionInMainFile(), hasOperatorName("!"),
                    hasUnaryOperand(ignoringParenImpCasts(
                        unaryOperator(hasOperatorName("!"), hasUnaryOperand(expr().bind("expr"))).bind("deref"))))
          .bind("double_negation"),
      changeTo(node("double_negation"), cat(node("expr"))));
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
} // namespace pancake::pass_simplify_double_negation
