#include "Pass_TransformLogicalExpressions.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
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

// reduce all logical expressions and expressions with side effects to temporary variables
namespace pancake::pass_hoist_condition_expressions {
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

  // process all OUTER statements first
  auto shouldTraversePostOrder() const -> bool { return false; }

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.pa_ctx.major_pass_number, data.pa_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto VisitIfStmt(IfStmt *ifStmt) -> bool {
    const auto *cond = ifStmt->getCond()->IgnoreParenImpCasts();
    if (!cond->HasSideEffects(data.Ctx)) {
      return true;
    }

    auto tmp_var_name = GetTempVarName("If");
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    os << llvm::formatv("int {0} = ({1});\n", tmp_var_name,
                        Lexer::getSourceText(CharSourceRange::getTokenRange(cond->getSourceRange()),
                                             data.Ctx.getSourceManager(), data.Ctx.getLangOpts())
                            .str());
    os << llvm::formatv("if ({0}) ", tmp_var_name);

    // we just want to replace the "if (COND)" part
    data.replacements.emplace_back(
        data.Ctx.getSourceManager(),
        CharSourceRange::getTokenRange(SourceRange(
            ifStmt->getIfLoc(),
            Lexer::getLocForEndOfToken(cond->getEndLoc(), 0, data.Ctx.getSourceManager(), data.Ctx.getLangOpts()))),
        os.str());

    return true;
  }

  auto VisitReturnStmt(ReturnStmt *returnStmt) -> bool {
    const auto *ret_expr = returnStmt->getRetValue();
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
    os << llvm::formatv("int {0} = ({1});\n", tmp_var_name,
                        Lexer::getSourceText(CharSourceRange::getTokenRange(cond->getSourceRange()),
                                             data.Ctx.getSourceManager(), data.Ctx.getLangOpts())
                            .str());
    os << llvm::formatv("return {0};", tmp_var_name);

    // we want to replace the "return EXPR;" part
    data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                   CharSourceRange::getTokenRange(returnStmt->getSourceRange()), os.str());

    return true;
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

  pa_ctx.failure_mode = FailureMode::RepeatPass;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.failure_mode = FailureMode::Success;
  }
}
} // namespace pancake::pass_hoist_condition_expressions

namespace pancake::pass_lower_nested_expressions {
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

  static auto CompoundAssignOpToOp(BinaryOperatorKind compound_assign_op) -> BinaryOperatorKind {
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
      llvm_unreachable("Not a compound assignment operator");
    }
  }

  struct BuiltExpr {
    llvm::SmallVector<std::string, 4> pre_stmts;
    std::string final_expr;
    QualType final_expr_type;
  };

  auto BuildExpr(Expr *expr, const size_t depth) -> BuiltExpr {
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
    }
    // CompoundAssignOperator is a specialization of BinaryOperator
    else if (auto *compound_assign_operator = dyn_cast<CompoundAssignOperator>(expr)) {
      auto *lhs = compound_assign_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = compound_assign_operator->getRHS()->IgnoreParenImpCasts();

      auto rhs_res = BuildExpr(rhs, depth + 1);
      auto lhs_res = BuildExpr(lhs, depth + 1);

      if (depth == 0) {
        // semi-colon is added by the caller
        os << llvm::formatv("{0} = {0} {1} {2}", lhs_res.final_expr,
                            BinaryOperator::getOpcodeStr(CompoundAssignOpToOp(compound_assign_operator->getOpcode())),
                            rhs_res.final_expr);
      } else {
        // we need to hoist this before
        pre_stmts.push_back(
            llvm::formatv("{0} = {0} {1} {2};\n", lhs_res.final_expr,
                          BinaryOperator::getOpcodeStr(CompoundAssignOpToOp(compound_assign_operator->getOpcode())),
                          rhs_res.final_expr)
                .str());
        os << lhs_res.final_expr;
      }
      pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());
    } else if (auto *conditional_operator = dyn_cast<ConditionalOperator>(expr)) {
      auto *cond = conditional_operator->getCond()->IgnoreParenImpCasts();
      auto *lhs = conditional_operator->getTrueExpr()->IgnoreParenImpCasts();
      auto *rhs = conditional_operator->getFalseExpr()->IgnoreParenImpCasts();

      auto cond_res = BuildExpr(cond, depth + 1);
      auto lhs_res = BuildExpr(lhs, depth + 1);
      auto rhs_res = BuildExpr(rhs, depth + 1);

      std::string tmp_var_name = GetTempVarName("TernaryResult");
      std::string tmp_cond_name = GetTempVarName("TernaryCond");
      std::string if_cond = llvm::formatv(
          /*
          0 = var for result of ?:
          1 = condition bool pre stmts
          2 = condition bool name
          3 = condition bool final expr
          4 = lhs pre stmts
          5 = lhs final expr
          6 = rhs pre stmts
          7 = rhs final expr
          8 = var for result of ?: (without type)
          */
          R"({0};
{1}
int {2} = {3};
if ({2}) {
  {4}
  {8} = {5};
} else {
  {6}
  {8} = {7};
}
)",
          PrintType(conditional_operator->getType(), tmp_var_name),
          llvm::join(cond_res.pre_stmts | std::views::reverse, "\n"), tmp_cond_name, cond_res.final_expr,
          llvm::join(lhs_res.pre_stmts | std::views::reverse, "\n"), lhs_res.final_expr,
          llvm::join(rhs_res.pre_stmts | std::views::reverse, "\n"), rhs_res.final_expr, tmp_var_name);

      pre_stmts.push_back(if_cond);
      os << tmp_var_name;
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
        // LAnd and LOr are handled specially because they short circuit
      case BO_LAnd: {
        auto lhs_res = BuildExpr(lhs, depth + 1);
        auto rhs_res = BuildExpr(rhs, depth + 1);
        std::string tmp_var_name = GetTempVarName("LAnd");
        std::string if_cond = llvm::formatv(
            /*
            0 = lhs pre stmts
            1 = tmp var for result of &&
            2 = lhs final expr
            3 = rhs pre stmts (only evaluated if lhs is true)
            4 = rhs final expr (only evaluated if lhs is true)
            */
            R"({0}
/* c2pancake: transformed logical && */
/* original expr: {5} */
int {1} = 0;
if (!({2})) {
  {1} = 0;
} else {
  {3}
  {1} = !!({4}); 
})",
            llvm::join(lhs_res.pre_stmts | std::views::reverse, "\n"), tmp_var_name, lhs_res.final_expr,
            llvm::join(rhs_res.pre_stmts | std::views::reverse, "\n"), rhs_res.final_expr,
            Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), data.Ctx.getSourceManager(),
                                 data.Ctx.getLangOpts())
                .str());

        pre_stmts.push_back(if_cond);
        os << tmp_var_name;
        break;
      }
      case BO_LOr: {
        auto lhs_res = BuildExpr(lhs, depth + 1);
        auto rhs_res = BuildExpr(rhs, depth + 1);
        std::string tmp_var_name = GetTempVarName("LOr");
        std::string if_cond = llvm::formatv(
            /*
            0 = lhs pre stmts
            1 = tmp var for result of ||
            2 = lhs final expr
            3 = rhs pre stmts (only evaluated if lhs is false)
            4 = rhs final expr (only evaluated if lhs is false)
            */
            R"({0}
/* c2pancake: transformed logical || */
/* original expr: {5} */
int {1} = 0;
if ({2}) {
  {1} = 1;
} else {
  {3}
  {1} = !!({4}); 
})",
            llvm::join(lhs_res.pre_stmts | std::views::reverse, "\n"), tmp_var_name, lhs_res.final_expr,
            llvm::join(rhs_res.pre_stmts | std::views::reverse, "\n"), rhs_res.final_expr,
            Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), data.Ctx.getSourceManager(),
                                 data.Ctx.getLangOpts())
                .str());
        pre_stmts.push_back(if_cond);
        os << tmp_var_name;
        break;
      }

      case BO_Assign: {
        auto rhs_res = BuildExpr(rhs, depth + 1);
        auto lhs_res = BuildExpr(lhs, depth + 1);

        if (depth == 0) {
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

      case BO_Comma: {
        // LHS is evaluated first, then RHS
        // RHS is returned

        auto rhs_res = BuildExpr(rhs, depth + 1);
        auto lhs_res = BuildExpr(lhs, depth + 1);
        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());

        os << rhs_res.final_expr;
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
        auto rhs_res = BuildExpr(rhs, depth + 1);
        auto lhs_res = BuildExpr(lhs, depth + 1);
        pre_stmts.insert(pre_stmts.end(), rhs_res.pre_stmts.begin(), rhs_res.pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res.pre_stmts.begin(), lhs_res.pre_stmts.end());

        // recursion level doesn't matter
        os << llvm::formatv("({0} {1} {2})", lhs_res.final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res.final_expr);
        break;
      }

      default: {
        llvm_unreachable("Unhandled binary operator");
        break;
      }
      }
    } else if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      auto res = BuildExpr(sub_expr, depth + 1);

      switch (unary_operator->getOpcode()) {
      case UO_PostInc:
        [[fallthrough]];
      case UO_PostDec: {
        if (depth == 0) {
          os << llvm::formatv("{0} = {0} {1} 1", res.final_expr, unary_operator->getOpcode() == UO_PostInc ? "+" : "-");
        } else {
          std::string tmp_var_name = GetTempVarName(
              llvm::formatv("{0}", unary_operator->getOpcode() == UO_PostInc ? "PostInc" : "PostDec").str());
          // push in REVERSE order!
          pre_stmts.push_back(
              llvm::formatv("{0} = {0} {1} 1;", res.final_expr, unary_operator->getOpcode() == UO_PostInc ? "+" : "-")
                  .str());
          pre_stmts.push_back(
              llvm::formatv("{0} = ({1});", PrintType(res.final_expr_type, tmp_var_name), res.final_expr).str());
          os << tmp_var_name;
        }
        break;
      }

      case UO_PreInc:
        [[fallthrough]];
      case UO_PreDec: {
        if (depth == 0) {
          os << llvm::formatv("{0} = {0} {1} 1", res.final_expr, unary_operator->getOpcode() == UO_PreInc ? "+" : "-");
        } else {
          std::string tmp_var_name = GetTempVarName(
              llvm::formatv("{0}", unary_operator->getOpcode() == UO_PreInc ? "PreInc" : "PreDec").str());
          pre_stmts.push_back(
              llvm::formatv("{0} = {0} {1} 1;", res.final_expr, unary_operator->getOpcode() == UO_PreInc ? "+" : "-")
                  .str());
          os << tmp_var_name;
        }
        break;
      }

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
        arg_built_exprs.push_back(BuildExpr(arg, depth + 1));
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

      auto res = BuildExpr(idx, depth + 1);
      pre_stmts.insert(pre_stmts.end(), res.pre_stmts.begin(), res.pre_stmts.end());
      os << llvm::formatv("{0}[{1}]",
                          Lexer::getSourceText(CharSourceRange::getTokenRange(base->getSourceRange()),
                                               data.Ctx.getSourceManager(), data.Ctx.getLangOpts()),
                          res.final_expr);
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

        auto res = BuildExpr(init_expr->IgnoreParenImpCasts(), 0);
        for (auto &&pre_stmt : res.pre_stmts | std::views::reverse) {
          os << pre_stmt << "\n";
        }
        os << llvm::formatv("{0} = {1};", PrintType(var_decl->getType(), var_decl->getName()), res.final_expr);
      }
    }

    os.flush();
    if (!replacement_text.empty()) {
      data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                     CharSourceRange::getTokenRange(declStmt->getSourceRange()), replacement_text);
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
      auto res = BuildExpr(expr, 0);
      os << llvm::join(res.pre_stmts | std::views::reverse, "\n");
      // for some reason we don't need a ; after this...
      os << llvm::formatv("{0}", res.final_expr);
      os.flush();

      if (!replacement_text.empty()) {
        data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                       CharSourceRange::getTokenRange(stmt->getSourceRange()), os.str());
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
} // namespace pancake::pass_lower_nested_expressions
