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
#include <string>
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
  bool is_on_heap;
  std::optional<size_t> alignment;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type, bool is_on_heap, std::optional<size_t> alignment = std::nullopt)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr(std::move(final_expr)),
        is_on_heap(is_on_heap), alignment(alignment) {}
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

        data.replacements.emplace_back(
            data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(var_decl->getSourceRange()),
            llvm::formatv("/* \"{0}\" at offset 0x{0:x} */", var_decl->getName(), addr).str(), data.Ctx.getLangOpts());
        continue;
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

    // TODO check function attributes
    // func_decl->getAttrs();

    auto *body = func_decl->getBody();
    if (body != nullptr) {
      if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(body)) {
        llvm::SmallVector<std::string> stmts;
        stmts.reserve(compound_stmt->size());
        for (auto *stmt : compound_stmt->body()) {
          auto res = BuildStmt(stmt);
          if (auto error = res.takeError()) {
            data.error = std::move(error);
            return false;
          }
          stmts.emplace_back(res.get());
        }

        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);
        os << "fun 1 " << func_decl->getName() << " (";
        for (auto &&param : func_decl->parameters()) {
          os << "1 " << param->getName();
          if (param != func_decl->parameters().back()) {
            os << ", ";
          }
        }
        os << ") {\n";
        for (auto &&stmt : stmts) {
          os << stmt << "\n";
        }
        os << "}\n";
        os.flush();
        data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                       CharSourceRange::getTokenRange(func_decl->getSourceRange()), replacement_text,
                                       data.Ctx.getLangOpts());
      } else {
        data.error = CreateRuntimeError("Function body is not a compound statement\n    at " +
                                        body->getBeginLoc().printToString(data.Ctx.getSourceManager()));
        return false;
      }
    }

    return false;
  }

  auto BuildStmt(Stmt *stmt) -> Expected<std::string> {
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(stmt)) {
      for (auto *sub_stmt : compound_stmt->body()) {
        auto result = BuildStmt(sub_stmt);
        if (auto error = result.takeError()) {
          return std::move(error);
        }

        os << result.get();
      }
      os.flush();

      return replacement_text;
    }

    if (auto *decl_stmt = llvm::dyn_cast<DeclStmt>(stmt)) {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      for (auto *decl : decl_stmt->decls()) {
        if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
          // TODO handle shapes and stuff in the future...
          os << "var 1 " << var_decl->getName() << ";\n";
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

  auto BuildExpr(const BuildExprCtx &ctx) -> Expected<BuiltExpr> {
    auto *expr = ctx.expr->IgnoreParenImpCasts();

    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const auto final_expr_type = expr->getType();

    if (auto *decl_ref_expr = dyn_cast<DeclRefExpr>(expr)) {
      if (auto *var_decl = dyn_cast<VarDecl>(decl_ref_expr->getDecl())) {
        if (data.global_var_map.contains(var_decl)) {
          auto addr = data.global_var_map[var_decl];
          auto bytes = (uint64_t)data.Ctx.getTypeSizeInChars(var_decl->getType()).getQuantity();

          switch (ctx.usage_kind) {
          case Usage::Value: {
            switch (bytes) {
            case 1:
              os << "ld8";
              break;
            case 4:
              os << "ld32";
              break;
            case 8:
              os << "lds 1";
              break;
            default:
              return CreateRuntimeError(std::move(
                  llvm::formatv("    at {0}\nUnsupported global variable size: {1} bytes",
                                decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), bytes)));
            }
            // TODO add support for shared loads
            os << llvm::formatv(" @base + {0}", addr);
            break;
          }
          case Usage::Place: {
            os << llvm::formatv("@base + {0}", addr);
            break;
          }
          default:
            return CreateRuntimeError(
                std::move(llvm::formatv("    at {0}\nUnsupported usage kind for global variable: {1}",
                                        decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                        static_cast<uint8_t>(ctx.usage_kind))));
          }
          os.flush();
          return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, true,
                           (size_t)data.Ctx.getTypeAlignInChars(var_decl->getType()).getQuantity());
        }

        // local var
        os << var_decl->getName();
        os.flush();
        return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
      }

      return CreateRuntimeError(
          std::move(llvm::formatv("    at {0}\nUnsupported declaration reference expression: {1}",
                                  decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                  decl_ref_expr->getDecl()->getDeclKindName())));
    }

    if (auto *integer_literal = dyn_cast<IntegerLiteral>(expr)) {
      // pancake only supports base 10 integer literals
      llvm::SmallString<16> integer_literal_str;
      // reserve enough space for the decimal representation
      integer_literal_str.reserve((integer_literal->getValue().getBitWidth() / 3) + 1);
      integer_literal->getValue().toString(integer_literal_str, 10, integer_literal->getType()->isSignedIntegerType());
      os << integer_literal_str;
      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
    }

    if (auto *character_literal = dyn_cast<CharacterLiteral>(expr)) {
      // turn into integers because pancake doesn't support character literals
      llvm::APInt value(32, character_literal->getValue());

      llvm::SmallString<16> integer_literal_str;
      // reserve enough space for the decimal representation
      integer_literal_str.reserve((value.getBitWidth() / 3) + 1);
      value.toString(integer_literal_str, 10, character_literal->getType()->isSignedIntegerType());
      os << integer_literal_str;
      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
    }

    if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      auto res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      // pancake doesn't have casts so we just ignore them
      os << res->final_expr;
      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, res->is_on_heap);
    }

    if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();

      switch (binary_operator->getOpcode()) {
      case BO_Assign: {
        auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nAssignment operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        if (lhs_res->is_on_heap) {
          if (!lhs_res->alignment.has_value()) {
            return CreateRuntimeError(
                std::move(llvm::formatv("    at {0}\nHeap variable has no alignment information",
                                        binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
          }

          switch (lhs_res->alignment.value()) {
          case 1:
            os << "st8";
            break;
          case 4:
            os << "st32";
            break;
          case 8:
            os << "st";
            break;
          default:
            return CreateRuntimeError(std::move(llvm::formatv(
                "    at {0}\nUnsupported global variable alignment: {1} bytes",
                binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()), lhs_res->alignment.value())));
          }
          // TODO add support for shared stores
          os << llvm::formatv(" {0}, {1}", lhs_res->final_expr, rhs_res->final_expr);
        } else {
          pre_stmts.push_back(llvm::formatv("{0} = {1};", lhs_res->final_expr, rhs_res->final_expr).str());
          if (ctx.usage_kind == Usage::Value) {
            os << lhs_res->final_expr;
          }
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
        auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
        if (auto error = rhs_res.takeError()) {
          return std::move(error);
        }
        auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Value));
        if (auto error = lhs_res.takeError()) {
          return std::move(error);
        }

        if (ctx.usage_kind == Usage::Place) {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nBinary operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

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

      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
    }

    if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();

      switch (unary_operator->getOpcode()) {
      case UO_Deref: {
        switch (ctx.usage_kind) {
        case Usage::Value: {
          auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
          if (auto error = sub_expr_res.takeError()) {
            return std::move(error);
          }
          // get size and alignment of the dereferenced type
          auto deref_type = sub_expr->getType()->getPointeeType();
          // auto align_bytes = (uint64_t)data.Ctx.getTypeAlignInChars(deref_type).getQuantity();
          auto size_bytes = (uint64_t)data.Ctx.getTypeSizeInChars(deref_type).getQuantity();
          switch (size_bytes) {
          case 1:
            os << "ld8";
            break;
          case 4:
            os << "ld32";
            break;
          case 8:
            os << "lds 1";
            break;
          default:
            return CreateRuntimeError(std::move(
                llvm::formatv("    at {0}\nUnsupported dereferenced type size: {1} bytes",
                              unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()), size_bytes)));
          }
          // TODO add support for shared loads
          os << llvm::formatv(" {0}", sub_expr_res->final_expr);
          pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
          break;
        }

        case Usage::Place: {
          // this branch happens if you have something like *p = 3; where p is a pointer to a variable
          // the *p is the expr here and the usage kind is place because it's the LHS of an assignment
          // we basically treat the *p as &p - funny how that works out

          auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place));
          if (auto error = sub_expr_res.takeError()) {
            return std::move(error);
          }
          os << sub_expr_res->final_expr;
          pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
          break;
        }
        default: {
          return CreateRuntimeError(
              std::move(llvm::formatv("    at {0}\nUnsupported usage kind for dereference operator: {1}",
                                      unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                      static_cast<uint8_t>(ctx.usage_kind))));
        }
        }
        break;
      }

      case UO_AddrOf: {
        auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place));
        if (auto error = sub_expr_res.takeError()) {
          return std::move(error);
        }
        os << sub_expr_res->final_expr;
        pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
        break;
      }

      case UO_Plus: {
        auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
        if (auto error = sub_expr_res.takeError()) {
          return std::move(error);
        }
        os << "(0 + " << sub_expr_res->final_expr << ")";
        pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
        break;
      }

      case UO_Minus: {
        auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
        if (auto error = sub_expr_res.takeError()) {
          return std::move(error);
        }
        os << "(0 - " << sub_expr_res->final_expr << ")";
        pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
        break;
      }

      case UO_Not: {
        auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
        if (auto error = sub_expr_res.takeError()) {
          return std::move(error);
        }
        os << "(-1 ^ " << sub_expr_res->final_expr << ")";
        pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
        break;
      }

      case UO_LNot: {
        auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
        if (auto error = sub_expr_res.takeError()) {
          return std::move(error);
        }
        os << "!" << sub_expr_res->final_expr;
        pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
        break;
      }
      default: {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nUnhandled unary operator: {1}",
                                    unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str())));
        break;
      }
      }

      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
    }

    if (auto *call_expr = dyn_cast<CallExpr>(expr)) {
      llvm::SmallVector<BuiltExpr, 4> arg_built_exprs;
      for (auto *arg : call_expr->arguments()) {
        auto res = BuildExpr(BuildExprCtx(arg->IgnoreParenImpCasts(), Usage::Value));
        if (auto error = res.takeError()) {
          return error;
        }
        arg_built_exprs.push_back(std::move(*res));
      }
      auto args_str = llvm::join(
          arg_built_exprs | std::views::transform([](const BuiltExpr &e) -> std::string { return e.final_expr; }),
          ", ");

      auto callee_name = call_expr->getDirectCallee() != nullptr ? call_expr->getDirectCallee()->getName().str() : "";
      if (callee_name.empty()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nFunction pointer calls are not supported",
                                    call_expr->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      if (callee_name.starts_with("__c2pnk_ffi_wrapper_")) {
        os << llvm::formatv("@{0}({1})", callee_name, args_str);
      } else {
        os << llvm::formatv("{0}({1})", callee_name, args_str);
      }
      for (auto &&built_expr : arg_built_exprs | std::views::reverse) {
        pre_stmts.insert(pre_stmts.end(), built_expr.pre_stmts.begin(), built_expr.pre_stmts.end());
      }

      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
    }

    if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      if (member_expr->isArrow()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("    at {0}\nArrow member access is not allowed here",
                                    member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()))));
      }

      // doesn't matter if the usage context is place or value, we just return the member offset
      auto *member_decl = member_expr->getMemberDecl();
      if (auto *field_decl = llvm::dyn_cast<FieldDecl>(member_decl)) {
        auto *record_decl = field_decl->getParent();
        const auto &layout = data.Ctx.getASTRecordLayout(record_decl);

        auto offset_in_bits = layout.getFieldOffset(field_decl->getFieldIndex());
        if (offset_in_bits % 8 != 0) {
          return CreateRuntimeError(std::move(llvm::formatv(
              "    at {0}\nUnsupported member access: {1} has an offset that is not a multiple of 8 bits",
              member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), field_decl->getName())));
        }
        auto offset_in_bytes = offset_in_bits / 8;

        auto member_base_expr = BuildExpr(BuildExprCtx(member_expr->getBase()->IgnoreParenImpCasts(), Usage::Place));
        if (auto error = member_base_expr.takeError()) {
          return error;
        }

        os << llvm::formatv("{0} + {1}", member_base_expr->final_expr, offset_in_bytes);
        os.flush();
        pre_stmts.insert(pre_stmts.end(), member_base_expr->pre_stmts.begin(), member_base_expr->pre_stmts.end());
        return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, member_base_expr->is_on_heap,
                         member_base_expr->alignment);
      }
      return CreateRuntimeError(std::move(llvm::formatv(
          "    at {0}\nUnsupported member access: {1}",
          member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), member_decl->getDeclKindName())));
    }

    if (auto error = PrintSourceText(os, expr, data.Ctx)) {
      return llvm::joinErrors(
          CreateRuntimeError(std::move(llvm::formatv("\n    at {0}\nFailed to print source text for expression",
                                                     expr->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
          std::move(error));
    }

    os.flush();
    return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, false);
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
