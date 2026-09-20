#include "Pass_C2Pancake.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Inclusions/HeaderIncludes.h>
#include <clang/Tooling/Inclusions/IncludeStyle.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <utility>

using namespace clang;
using namespace clang::tooling;

namespace pancake::pass_c2pancake {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  llvm::DenseMap<VarDecl *, uint64_t> &global_var_map;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::Error error = llvm::Error::success();
  uint64_t next_global_var_addr = 0x0UL;
  size_t tmp_var_counter = 0;
};

enum class Usage : uint8_t {
  /**
   * @brief Compute the expression's resulting value
   *
   * Used when: The caller needs the result
   *
   * Example: x = a + b, foo(expr), int y = (b = 3)
   */
  Value,

  /**
   * @brief Compute the reusable description of the storage location of the expression. Note this is very specifically
   * not supposed to return a pointer!
   *
   * Used when: The caller needs to read/write memory at that location
   *
   * Example: x = 3 (lhs), *p = v, ++a->b
   */
  Place,

  /**
   * @brief Execute the expression only for its side effects; ignore its value
   *
   * Used when: The result is thrown away
   *
   * Example: a = 3;, foo(expr);, ++a;
   */
  Effect
};

struct BuildExprCtx {
  Expr *expr;
  Usage usage_kind;
  explicit BuildExprCtx(Expr *expr, Usage usage_kind) : expr(expr), usage_kind(usage_kind) {}
};

struct BuiltExpr {
  llvm::SmallVector<std::string, 8> pre_stmts;
  clang::QualType final_expr_type;
  std::string final_expr;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr(std::move(final_expr)) {}
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return true; }

  auto TraverseTranslationUnitDecl(TranslationUnitDecl *tu_decl) -> bool {
    if (data.error) {
      return false;
    }

    for (auto *decl : tu_decl->decls()) {
      if (auto *func_decl = llvm::dyn_cast<FunctionDecl>(decl)) {
        if (!func_decl->isThisDeclarationADefinition()) {
          continue;
        }

        if (!TraverseFunctionDecl(func_decl)) {
          return false;
        }
        continue;
      }

      if (auto *_ = llvm::dyn_cast<TypedefDecl>(decl)) {
        continue;
      }

      if (auto *_ = llvm::dyn_cast<RecordDecl>(decl)) {
        continue;
      }

      if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
        if (!var_decl->hasGlobalStorage()) {
          data.error = CreateRuntimeError("Non-global variable declaration at top-level\n    at " +
                                          var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
          return false;
        }

        auto align_bytes = (uint64_t)data.Ctx.getTypeAlignInChars(var_decl->getType()).getQuantity();
        auto size_bytes = (uint64_t)data.Ctx.getTypeSizeInChars(var_decl->getType()).getQuantity();
        auto addr = data.next_global_var_addr;
        // round up addr to the next multiple of align_bytes
        addr = (addr + align_bytes - 1) / align_bytes * align_bytes;
        data.next_global_var_addr = addr + size_bytes;

        if (data.global_var_map.contains(var_decl)) {
          data.error = CreateRuntimeError("Duplicate global variable declaration at top-level\n    at " +
                                          var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
          return false;
        }

        data.global_var_map[var_decl] = addr;
        // TODO replace with comment that includes the address
      }

      data.error = CreateRuntimeError("Unexpected top-level declaration\n    at " +
                                      decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
      return false;
    }

    return true;
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    if (!func_decl->isThisDeclarationADefinition()) {
      return true;
    }

    auto *body = func_decl->getBody();
    if (body != nullptr) {
      if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(body)) {
        llvm::SmallVector<std::string> stmts;
        stmts.reserve(compound_stmt->size());
        for (auto *stmt : compound_stmt->body()) {
          auto res = TraverseStmt(stmt);
          if (auto error = res.takeError()) {
            data.error = std::move(error);
            return false;
          }
          stmts.emplace_back(res.get());
        }
        // TODO output pancake equivalent of the function
        // the body should already be handled by TraverseStmt
      } else {
        data.error = CreateRuntimeError("Function body is not a compound statement\n    at " +
                                        body->getBeginLoc().printToString(data.Ctx.getSourceManager()));
        return false;
      }
    }

    return false;
  }

  auto TraverseStmt(Stmt *stmt) -> Expected<std::string> {
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(stmt)) {
      for (auto *sub_stmt : compound_stmt->body()) {
        auto result = TraverseStmt(sub_stmt);
        if (auto error = result.takeError()) {
          return std::move(error);
        }

        os << *result;
      }
      os.flush();

      return replacement_text;
    }

    if (auto *decl_stmt = llvm::dyn_cast<DeclStmt>(stmt)) {
      for (auto *decl : decl_stmt->decls()) {
        if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
          // TODO handle var decl
          continue;
        }

        return CreateRuntimeError("Unexpected declaration statement\n    at " +
                                  decl->getBeginLoc().printToString(data.Ctx.getSourceManager()));
      }

      return replacement_text;
    }

    return CreateRuntimeError("Unexpected statement\n    at " +
                              stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()));
  }

  auto TraverseExpr(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    auto *expr = ctx.expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const auto final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      // TODO put the vars in the heap if necessary
      if (auto error = PrintSourceText(os, decl_ref_expr, data.Ctx)) {
        return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                    "\n    at {0}\nFailed to print source text for DeclRefExpr",
                                    decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager())))),
                                std::move(error));
      }
    } else if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      // pancake only supports base 10 integer literals
      llvm::SmallString<16> integer_literal_str;
      // reserve enough space for the decimal representation
      integer_literal_str.reserve(integer_literal->getValue().getBitWidth() / 3 + 1);
      integer_literal->getValue().toString(integer_literal_str, 10, integer_literal->getType()->isSignedIntegerType());
      os << integer_literal_str;
    } else if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      // turn into integers because pancake doesn't support character literals
      llvm::APInt value(32, character_literal->getValue());

      llvm::SmallString<16> integer_literal_str;
      // reserve enough space for the decimal representation
      integer_literal_str.reserve(value.getBitWidth() / 3 + 1);
      value.toString(integer_literal_str, 10, character_literal->getType()->isSignedIntegerType());
      os << integer_literal_str;
    } else if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto res = TraverseExpr(BuildExprCtx(sub_expr, Usage::Value));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      // pancake doesn't have casts so we just ignore them
      os << res->final_expr;
      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
    } else if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
      case BO_Assign: {
        auto rhs_res = TraverseExpr(BuildExprCtx(rhs, Usage::Value));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_res = TraverseExpr(BuildExprCtx(lhs, Usage::Place));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nAssignment operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        // TODO deal with arrays and other stuff
        pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res->final_expr, rhs_res->final_expr).str());
        if (ctx.usage_kind == Usage::Value) {
          os << lhs_res->final_expr;
        }

        pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
        break;
      }

      case BO_Div: {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nPancake does not support the division operator!",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      case BO_Rem: {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nPancake does not support the modulo operator!",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      case BO_LAnd:
        [[fallthrough]];
      case BO_LOr:
        [[fallthrough]];
      case BO_Mul:
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
        auto rhs_res = TraverseExpr(BuildExprCtx(rhs, Usage::Value));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_res = TraverseExpr(BuildExprCtx(lhs, Usage::Value));
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
      case UO_Deref: {
        auto res = TraverseExpr(BuildExprCtx(sub_expr, Usage::Value));
        if (auto error = res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          // this expr is the LHS of an assignment
          os << res->final_expr;
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
      if (auto error = PrintSourceText(os, expr, data.Ctx)) {
        return llvm::joinErrors(CreateRuntimeError(std::move(
                                    llvm::formatv("\n    at {0}\nFailed to print source text for expression",
                                                  expr->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                std::move(error));
      }
    }

  build_expr_end:
    os.flush();
    return BuiltExpr(pre_stmts, final_expr, final_expr_type);
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  llvm::DenseMap<VarDecl *, uint64_t> global_var_map;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .global_var_map = global_var_map, .replacements = replacements};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }

  for (const auto &r : replacements) {
    if (auto error = ps_ctx.replacements.add(r)) {
      ps_ctx.error = llvm::joinErrors(CreateRuntimeError("Add replacement conflict"), std::move(error));
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_c2pancake
