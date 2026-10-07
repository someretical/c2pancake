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
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;

extern llvm::cl::opt<uint64_t> stack_size_opt;
extern llvm::cl::opt<std::string> make_path;
extern llvm::cl::opt<std::string> cake_path;
extern llvm::cl::opt<std::string> cake_options;

namespace pancake::pass_c2pancake {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  StageContext &ps_ctx;
  llvm::raw_string_ostream &tu_os;
  llvm::raw_ostream &ffi_os;
  llvm::DenseMap<VarDecl *, uint64_t> &global_var_map;
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
  std::optional<size_t> heap_alignment;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type, std::optional<size_t> heap_alignment)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr(std::move(final_expr)),
        heap_alignment(heap_alignment) {}
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

      if (auto *_ = llvm::dyn_cast<EnumDecl>(decl)) {
        continue;
      }

      if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
        if (!var_decl->hasGlobalStorage()) {
          data.error = CreateRuntimeError(std::move(
              llvm::formatv("\n    at {0}\nNon-global variable declaration at top-level: {1}",
                            var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName())));
          return false;
        }

        auto align_bytes = (uint64_t)data.Ctx.getTypeAlignInChars(var_decl->getType()).getQuantity();
        auto size_bytes = (uint64_t)data.Ctx.getTypeSizeInChars(var_decl->getType()).getQuantity();
        auto addr = data.next_global_var_addr;
        // round up addr to the next multiple of align_bytes
        addr = (addr + align_bytes - 1) / align_bytes * align_bytes;
        data.next_global_var_addr = addr + size_bytes;

        if (data.global_var_map.contains(var_decl)) {
          data.error = CreateRuntimeError(std::move(
              llvm::formatv("\n    at {0}\nDuplicate global variable declaration at top-level: {1}",
                            var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName())));
          return false;
        }

        data.global_var_map[var_decl] = addr;
        // TODO replace with comment that includes the address

        data.tu_os << llvm::formatv("/* \"{0}\" at offset {1:x} */\n", var_decl->getName(), addr);
        continue;
      }

      data.error = CreateRuntimeError(std::move(
          llvm::formatv("\n    at {0}\nUnexpected top-level declaration: {1}",
                        decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), decl->getDeclKindName())));
      return false;
    }

    if (auto error = data.ps_ctx.run.artifacts.Set(heap_size_key, HeapSize{data.next_global_var_addr})) {
      data.error = std::move(error);
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

    if (FuncIsFFIWrapper(func_decl)) {
      // grab annotation containing the include path
      auto include_path = GetFFIFuncIncludePath(func_decl);
      if (auto error = include_path.takeError()) {
        data.error = std::move(error);
        return false;
      }
      data.ffi_os << llvm::formatv("#include <{0}>\n", include_path.get());

      auto src_text = Lexer::getSourceText(CharSourceRange::getTokenRange(func_decl->getSourceRange()),
                                           data.Ctx.getSourceManager(), data.Ctx.getLangOpts())
                          .str();
      // append all __c2pnk_ffi_wrapper with "ffi" in front
      size_t pos = 0;
      while ((pos = src_text.find("__c2pnk_ffi_wrapper_", pos)) != std::string::npos) {
        src_text.replace(pos, strlen("__c2pnk_ffi_wrapper_"), "ffi__c2pnk_ffi_wrapper_");
        pos += strlen("ffi__c2pnk_ffi_wrapper_");
      }

      // grab src code for func_decl and write to ffi_os
      data.ffi_os << src_text << "\n";
      return true;
    }

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
          if (!res.get().empty()) {
            stmts.emplace_back(res.get());
          }
        }

        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);

        // TODO Pancake playground does not support this yet!
        if (func_decl->isInlineSpecified()) {
          os << "inline ";
        }

        os << "fun 1 " << func_decl->getName() << "(";
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

        data.tu_os << replacement_text;
      } else {
        data.error = CreateRuntimeError(std::move(
            llvm::formatv("\n    at {0}\nFunction body is not a compound statement: {1}",
                          body->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName())));
        return false;
      }
    }

    return true;
  }

  auto GetFFIFuncIncludePath(FunctionDecl *func_decl) -> Expected<std::string> {
    auto &attrs = func_decl->getAttrs();
    for (auto *attr : attrs) {
      if (auto *annotate_attr = llvm::dyn_cast<AnnotateAttr>(attr)) {
        auto annotation = annotate_attr->getAnnotation();
        if (annotation.starts_with("__c2pnk_ffi_include_path_")) {
          return annotation.substr(strlen("__c2pnk_ffi_include_path_")).str();
        }
      }
    }
    return CreateRuntimeError(std::move(
        llvm::formatv("\n    at {0}\nFunction {1} does not have an include path annotation",
                      func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName())));
  }

  static auto FuncIsFFIWrapper(FunctionDecl *func_decl) -> bool {
    return func_decl->getName().starts_with("__c2pnk_ffi_wrapper_");
  }

  auto BuildStmt(Stmt *stmt, bool needs_semicolon = true) -> Expected<std::string> {
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);
    if (auto *compound_stmt = llvm::dyn_cast<CompoundStmt>(stmt)) {
      bool add_curly_braces = true;

      if (compound_stmt->size() == 1) {
        if (llvm::isa<CompoundStmt>(compound_stmt->body_front())) {
          add_curly_braces = false;
        }
      }

      if (add_curly_braces) {
        os << "{\n";
      }
      for (auto *sub_stmt : compound_stmt->body()) {
        auto result = BuildStmt(sub_stmt);
        if (auto error = result.takeError()) {
          return std::move(error);
        }

        os << result.get() << "\n";
      }
      if (add_curly_braces) {
        os << "}";
      }
      // pancake is special because standalone block scopes need semi colons after
      // but if the block is part of a control flow statement, it doesn't need a semi colon after
      if (needs_semicolon) {
        os << ";";
      }
      os << "\n";
      os.flush();

      return replacement_text;
    }

    if (auto *decl_stmt = llvm::dyn_cast<DeclStmt>(stmt)) {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      for (auto *decl : decl_stmt->decls()) {
        if (auto *var_decl = llvm::dyn_cast<VarDecl>(decl)) {
          if (var_decl->hasInit()) {
            auto *init_expr = var_decl->getInit();
            auto res = BuildExpr(BuildExprCtx(init_expr, Usage::Value));
            if (auto error = res.takeError()) {
              return std::move(error);
            }

            for (auto &&pre_stmt : res->pre_stmts) {
              os << pre_stmt << "\n";
            }
            os << "var 1 " << var_decl->getName() << " = " << res->final_expr << ";";
          } else {
            // TODO handle shapes and stuff in the future...
            os << "var 1 " << var_decl->getName() << " = 0;";
          }
          continue;
        }

        return CreateRuntimeError(std::move(
            llvm::formatv("\n    at {0}\nUnexpected declaration statement: {1}",
                          decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), decl->getDeclKindName())));
      }

      return replacement_text;
    }

    if (auto *return_stmt = llvm::dyn_cast<ReturnStmt>(stmt)) {
      auto *ret_expr = return_stmt->getRetValue();
      if (ret_expr == nullptr) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nReturn statement has no return value",
                                    return_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()))));
      }

      auto res = BuildExpr(BuildExprCtx(ret_expr, Usage::Value));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      for (auto &&pre_stmt : res->pre_stmts) {
        os << pre_stmt << "\n";
      }
      os << "return " << res->final_expr << ";";
      os.flush();
      return replacement_text;
    }

    if (auto *if_stmt = llvm::dyn_cast<IfStmt>(stmt)) {
      auto *cond = if_stmt->getCond();
      auto res = BuildExpr(BuildExprCtx(cond, Usage::Value));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      auto *then_stmt = if_stmt->getThen();
      auto then_res = BuildStmt(then_stmt, false);
      if (auto error = then_res.takeError()) {
        return std::move(error);
      }

      for (auto &&pre_stmt : res->pre_stmts) {
        os << pre_stmt << "\n";
      }
      if (llvm::isa<CompoundStmt>(then_stmt)) {
        os << "if (" << res->final_expr << ") ";
        os << then_res.get();
      } else {
        os << "if (" << res->final_expr << ") {\n";
        os << then_res.get();
        os << "\n}";
      }

      if (auto *else_stmt = if_stmt->getElse()) {
        auto else_res = BuildStmt(else_stmt, false);
        if (auto error = else_res.takeError()) {
          return std::move(error);
        }

        if (llvm::isa<CompoundStmt>(else_stmt)) {
          os << " else ";
          os << else_res.get();
        } else {
          os << " else {\n";
          os << else_res.get();
          os << "\n}";
        }
      }

      os.flush();
      return replacement_text;
    }

    if (auto *while_stmt = llvm::dyn_cast<WhileStmt>(stmt)) {
      auto *cond = while_stmt->getCond();
      auto res = BuildExpr(BuildExprCtx(cond, Usage::Value));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      auto *body_stmt = while_stmt->getBody();
      auto body_res = BuildStmt(body_stmt, false);
      if (auto error = body_res.takeError()) {
        return std::move(error);
      }

      for (auto &&pre_stmt : res->pre_stmts) {
        os << pre_stmt << "\n";
      }
      if (llvm::isa<CompoundStmt>(body_stmt)) {
        os << "while (" << res->final_expr << ") ";
        os << body_res.get();
      } else {
        os << "while (" << res->final_expr << ") {\n";
        os << body_res.get();
        os << "\n}";
      }
      os.flush();
      return replacement_text;
    }

    if (auto *_ = llvm::dyn_cast<BreakStmt>(stmt)) {
      os << "break;";
      os.flush();
      return replacement_text;
    }

    if (auto *_ = llvm::dyn_cast<NullStmt>(stmt)) {
      return replacement_text;
    }

    if (auto *expr = llvm::dyn_cast<Expr>(stmt)) {
      if (!expr->HasSideEffects(data.Ctx)) {
        // Ignore the unused result
        return replacement_text;
      }

      auto res = BuildExpr(BuildExprCtx(expr, Usage::Effect));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      for (auto &&pre_stmt : res->pre_stmts) {
        os << pre_stmt << "\n";
      }
      if (!res->final_expr.empty()) {
        os << res->final_expr << ";";
      }
      os.flush();
      return replacement_text;
    }

    return CreateRuntimeError(llvm::formatv("\n    at {0}\nUnexpected statement {1}", stmt->getStmtClassName(),
                                            stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())));
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

          auto usage_kind = ctx.usage_kind;
          // if type is array, just decay to pointer AKA place
          if (final_expr_type->isArrayType()) {
            usage_kind = Usage::Place;
          }

          // std::optional<size_t> heap_alignment = std::nullopt;

          switch (usage_kind) {
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
              return CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nUnsupported global variable {1} size: {2} bytes",
                  decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName(), bytes)));
            }
            // TODO add support for shared loads
            os << llvm::formatv(" (@base + {0})", addr);
            break;
          }
          case Usage::Place: {
            os << llvm::formatv("@base + {0}", addr);
            break;
          }
          default:
            return CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nUnsupported usage kind for global variable ({1}): {2}",
                                        decl_ref_expr->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                        var_decl->getName(), static_cast<uint8_t>(ctx.usage_kind))));
          }
          os.flush();
          return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type,
                           (size_t)data.Ctx.getTypeAlignInChars(var_decl->getType()).getQuantity());
        }

        // local var
        os << var_decl->getName();
        os.flush();
        return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
      }

      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nUnsupported declaration reference expression: {1}",
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
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
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
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
    }

    if (auto *c_style_cast_expr = dyn_cast<CStyleCastExpr>(expr)) {
      auto *sub_expr = c_style_cast_expr->getSubExpr();
      // pass the usage kind down
      auto res = BuildExpr(BuildExprCtx(sub_expr, ctx.usage_kind));
      if (auto error = res.takeError()) {
        return std::move(error);
      }

      // pancake doesn't have casts so we just ignore them
      os << res->final_expr;
      pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
      os.flush();
      // pass the heap alignment back up
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, res->heap_alignment);
    }

    if (auto *binary_operator = dyn_cast<BinaryOperator>(expr)) {
      auto *lhs = binary_operator->getLHS()->IgnoreParenImpCasts();
      auto *rhs = binary_operator->getRHS()->IgnoreParenImpCasts();
      std::optional<size_t> heap_alignment = std::nullopt;

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
              std::move(llvm::formatv("\n    at {0}\nAssignment operator cannot be used as a place expression",
                                      binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
        }

        if (auto heap_alignment = lhs_res->heap_alignment) {
          switch (heap_alignment.value()) {
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
            return CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nUnsupported global variable alignment: {1} bytes",
                                        binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                        lhs_res->heap_alignment.value())));
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
            std::move(llvm::formatv("\n    at {0}\nPancake does not support the division operator!",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }
      case BO_Rem: {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nPancake does not support the modulo operator!",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
      }

      case BO_Add: {
        auto res = BuildAddOp(expr, binary_operator);
        if (auto error = res.takeError()) {
          return std::move(error);
        }
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        os << res->final_expr;
        heap_alignment = res->heap_alignment;
        break;
      }

      case BO_Sub: {
        auto res = BuildSubOp(expr, binary_operator);
        if (auto error = res.takeError()) {
          return std::move(error);
        }
        pre_stmts.insert(pre_stmts.end(), res->pre_stmts.begin(), res->pre_stmts.end());
        os << res->final_expr;
        heap_alignment = res->heap_alignment;
        break;
      }

      case BO_LAnd:
        [[fallthrough]];
      case BO_LOr:
        [[fallthrough]];
      case BO_Mul:
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
          if (!binary_operator->isAdditiveOp()) {
            return CreateRuntimeError(std::move(
                llvm::formatv("\n    at {0}\nNon-additive binary operator ({1}) cannot be used as a place expression",
                              binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                              BinaryOperator::getOpcodeStr(binary_operator->getOpcode()))));
          }
          // Binary operator COULD be used as a place expression if it is a pointer arithmetic operation
        }

        pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
        pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
        os << llvm::formatv("({0} {1} {2})", lhs_res->final_expr,
                            BinaryOperator::getOpcodeStr(binary_operator->getOpcode()), rhs_res->final_expr);
        heap_alignment = std::nullopt;
        break;
      }

      default: {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nUnhandled binary operator: {1}",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    BinaryOperator::getOpcodeStr(binary_operator->getOpcode()))));
        break;
      }
      }

      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, heap_alignment);
    }

    if (auto *unary_operator = dyn_cast<UnaryOperator>(expr)) {
      auto *sub_expr = unary_operator->getSubExpr()->IgnoreParenImpCasts();
      std::optional<size_t> heap_alignment = std::nullopt;

      switch (unary_operator->getOpcode()) {
      case UO_Deref: {
        // An array-valued dereference decays to the address of the first
        // element when it is used as a value. It must not be emitted as a
        // scalar load: arrays cannot be loaded as a single Pancake value.
        const auto usage_kind = final_expr_type->isArrayType() ? Usage::Place : ctx.usage_kind;
        switch (ctx.usage_kind) {
        case Usage::Value: {
          if (usage_kind == Usage::Place) {
            auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place));
            if (auto error = sub_expr_res.takeError()) {
              return std::move(error);
            }
            os << sub_expr_res->final_expr;
            pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
            heap_alignment =
                (size_t)data.Ctx.getTypeAlignInChars(sub_expr->getType()->getPointeeType()).getQuantity();
            break;
          }
          auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Place));
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
                llvm::formatv("\n    at {0}\nUnsupported dereferenced type size: {1} bytes",
                              unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()), size_bytes)));
          }
          // TODO add support for shared loads
          os << llvm::formatv(" ({0})", sub_expr_res->final_expr);
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
          heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(sub_expr->getType()->getPointeeType()).getQuantity();
          break;
        }
        default: {
          return CreateRuntimeError(
              std::move(llvm::formatv("\n    at {0}\nUnsupported usage kind for dereference operator: {1}",
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
            std::move(llvm::formatv("\n    at {0}\nUnhandled unary operator: {1}",
                                    unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()),
                                    UnaryOperator::getOpcodeStr(unary_operator->getOpcode()).str())));
        break;
      }
      }

      os.flush();
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, heap_alignment);
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
            std::move(llvm::formatv("\n    at {0}\nFunction pointer calls are not supported",
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
      return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
    }

    if (auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      if (member_expr->isArrow()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nArrow member access is not allowed here",
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
              "\n    at {0}\nUnsupported member access: {1} has an offset that is not a multiple of 8 bits",
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
        return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, member_base_expr->heap_alignment);
      }
      return CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nUnsupported member access: {1}",
          member_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), member_decl->getDeclKindName())));
    }

    if (auto error = PrintSourceText(os, expr, data.Ctx)) {
      return llvm::joinErrors(
          CreateRuntimeError(std::move(llvm::formatv("\n    at {0}\nFailed to print source text for expression",
                                                     expr->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
          std::move(error));
    }

    os.flush();
    return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
  }

  auto BuildAddOp(Expr *expr, const BinaryOperator *binary_operator) -> Expected<BuiltExpr> {
    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const auto final_expr_type = expr->getType();
    std::optional<size_t> heap_alignment = std::nullopt;

    // it is very important that we do NOT call IgnoreParenImpCasts() on the lhs and rhs here
    // as we do NOT want to strip the implicit cast from array to pointer
    auto *lhs = binary_operator->getLHS();
    auto *rhs = binary_operator->getRHS();
    if (lhs->getType()->isPointerType() && rhs->getType()->isIntegerType()) {
      // p + 1 is valid

      // strip pointer from lhs and get size of the type
      auto width = data.Ctx.getTypeSizeInChars(lhs->getType()->getPointeeType()).getQuantity();

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Place));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Value));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }
      os << llvm::formatv("({0} + ({1} * {2}))", lhs_res->final_expr, rhs_res->final_expr, width);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
      heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(lhs->getType()->getPointeeType()).getQuantity();
    } else if (lhs->getType()->isIntegerType() && rhs->getType()->isPointerType()) {
      // 1 + p is valid

      // strip pointer from rhs and get size of the type
      auto width = data.Ctx.getTypeSizeInChars(rhs->getType()->getPointeeType()).getQuantity();

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }
      os << llvm::formatv("({0} + ({1} * {2}))", rhs_res->final_expr, lhs_res->final_expr, width);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
      heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(rhs->getType()->getPointeeType()).getQuantity();
    } else if (lhs->getType()->isPointerType() && rhs->getType()->isPointerType()) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nInvalid pointer arithmetic: pointer + pointer",
                                  binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
    } else {
      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Value));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }
      os << llvm::formatv("({0} + {1})", lhs_res->final_expr, rhs_res->final_expr);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
    }

    os.flush();
    return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, heap_alignment);
  }

  auto BuildSubOp(Expr *expr, const BinaryOperator *binary_operator) -> Expected<BuiltExpr> {
    llvm::SmallVector<std::string, 8> pre_stmts;
    std::string final_expr;
    llvm::raw_string_ostream os(final_expr);
    const auto final_expr_type = expr->getType();
    std::optional<size_t> heap_alignment = std::nullopt;

    // it is very important that we do NOT call IgnoreParenImpCasts() on the lhs and rhs here
    // as we do NOT want to strip the implicit cast from array to pointer
    auto *lhs = binary_operator->getLHS();
    auto *rhs = binary_operator->getRHS();
    if (lhs->getType()->isPointerType() && rhs->getType()->isIntegerType()) {
      // p - 1 is valid

      // strip pointer from lhs and get size of the type
      auto width = data.Ctx.getTypeSizeInChars(lhs->getType()->getPointeeType()).getQuantity();

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }

      os << llvm::formatv("({0} - ({1} * {2}))", lhs_res->final_expr, rhs_res->final_expr, width);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
      heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(lhs->getType()->getPointeeType()).getQuantity();
    } else if (lhs->getType()->isPointerType() && rhs->getType()->isPointerType()) {
      // p - q is valid (but we cannot handle this case because pancake doesn't have division...)
      // however, we can handle it if the size is a power of 2
      // return type is meant to be a ptrdiff_t, but we don't have that type in pancake so we just return a uint

      auto width = data.Ctx.getTypeSizeInChars(lhs->getType()->getPointeeType()).getQuantity();
      if ((width & (width - 1)) != 0) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nInvalid pointer arithmetic: pointer - pointer (unsupported size {1} "
                                    "bytes, not a power of 2)",
                                    binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()), width)));
      }

      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Place));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Place));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }

      auto shift_amount = llvm::Log2_64((uint64_t)width);
      os << llvm::formatv("(({0} - {1}) >> {2})", lhs_res->final_expr, rhs_res->final_expr, shift_amount);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
      heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(lhs->getType()->getPointeeType()).getQuantity();
    } else if (lhs->getType()->isIntegerType() && rhs->getType()->isPointerType()) {
      return CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nInvalid pointer arithmetic: integer - pointer",
                                  binary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()))));
    } else {
      auto rhs_res = BuildExpr(BuildExprCtx(rhs, Usage::Value));
      if (auto error = rhs_res.takeError()) {
        return std::move(error);
      }
      auto lhs_res = BuildExpr(BuildExprCtx(lhs, Usage::Value));
      if (auto error = lhs_res.takeError()) {
        return std::move(error);
      }
      os << llvm::formatv("({0} - {1})", lhs_res->final_expr, rhs_res->final_expr);
      pre_stmts.insert(pre_stmts.end(), rhs_res->pre_stmts.begin(), rhs_res->pre_stmts.end());
      pre_stmts.insert(pre_stmts.end(), lhs_res->pre_stmts.begin(), lhs_res->pre_stmts.end());
    }

    os.flush();
    return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, heap_alignment);
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  std::string tu_replacement_text;
  std::string ffi_replacement_text;
  llvm::raw_string_ostream tu_os(tu_replacement_text);
  llvm::raw_string_ostream ffi_os(ffi_replacement_text);

  llvm::DenseMap<VarDecl *, uint64_t> global_var_map;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .tu_os = tu_os, .ffi_os = ffi_os, .global_var_map = global_var_map};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  tu_os.flush();
  ffi_os.flush();

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }

  if (auto error = ps_ctx.run.artifacts.Set(translation_unit_output_key,
                                            TranslationUnitOutput{.content = std::move(tu_replacement_text)})) {
    ps_ctx.error = std::move(error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }
  if (auto error = ps_ctx.run.artifacts.Set(ffi_output_key, FFIOutput{.content = std::move(ffi_replacement_text)})) {
    ps_ctx.error = std::move(error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }

  ps_ctx.SetControl(StageControl::Continue);
}

namespace {
/*
Variables to replace:
- {0} total memory
- {1} heap size
- {2} stack size
*/
const char *SCAFFOLD_TEMPLATE = R"(
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static char cml_memory[{0}];
extern void *cml_heap;
extern void *cml_stack;
extern void *cml_stackend;
extern void cml_main(void);

void cml_exit(int arg) {{
  exit(arg);
}

void cml_err(int arg) {{
  if (arg == 3) {{
    fprintf(stderr,
      "Memory not ready for entry. "
      "You may have not run the init code yet, "
      "or be trying to enter during an FFI call.\\n");
  }
  cml_exit(arg);
}

void cml_clear() {{}

static void init_pancake_mem(void) {{
  unsigned long cml_heap_sz  = {1};
  unsigned long cml_stack_sz = {2};
  cml_heap     = cml_memory;
  cml_stack    = (unsigned char *)cml_heap + cml_heap_sz;
  cml_stackend = (unsigned char *)cml_stack + cml_stack_sz;
}

int main(void) {{
  init_pancake_mem();
  cml_main();
  return 0;
}
)";

/*
Variables to replace:
- {0} FFI functions
*/
const char *FFI_TEMPLATE = R"(
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

{0}
)";

/*
Note: recipe lines must begin with a literal tab, not spaces.

Variables to replace:
- {0} cake path
- {1} cake options
- {2} name
*/
const char *MAKEFILE_TEMPLATE = R"(
CAKE   ?= {0}
CC     ?= cc

CAKE_FLAGS = {1}

PANCAKE_SRC = {2}.pancake
SCAFFOLD    = {2}.scaffold.c
FFI         = {2}.ffi.c
ASM         = {2}.S
BIN         = {2}.bin

.PHONY: all run clean

all: $(BIN)

$(ASM): $(PANCAKE_SRC)
	cpp -P < $< | $(CAKE) $(CAKE_FLAGS) > $@

$(BIN): $(ASM) $(SCAFFOLD) $(FFI)
	$(CC) -o $@ $^

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(ASM) $(BIN)
)";
} // namespace

auto Finalizer::Write(StageContext &ctx, clang::CompilerInstance &compiler) -> llvm::Error {
  auto &tu_str = ctx.run.artifacts.Get(translation_unit_output_key)->content;
  auto &ffi_str = ctx.run.artifacts.Get(ffi_output_key)->content;
  auto heap_size = ctx.run.artifacts.Get(heap_size_key)->size;
  auto &stack_size = stack_size_opt;

  auto tu_file_path = ctx.next_file + ".pancake";
  auto scaffold_file_path = ctx.next_file + ".scaffold.c";
  auto ffi_file_path = ctx.next_file + ".ffi.c";
  auto makefile_path = ctx.next_file + ".makefile";
  auto bin_path = ctx.next_file + ".bin";
  auto asm_path = ctx.next_file + ".S";

  std::error_code ec;
  llvm::raw_fd_ostream tu_out(tu_file_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return CreateRuntimeError(llvm::formatv("\nFailed to open TU output file {0}: {1}", tu_file_path, ec.message()));
  }

  llvm::raw_fd_ostream scaffold_out(scaffold_file_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return CreateRuntimeError(
        llvm::formatv("\nFailed to open scaffold output file {0}: {1}", scaffold_file_path, ec.message()));
  }

  llvm::raw_fd_ostream ffi_out(ffi_file_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return CreateRuntimeError(llvm::formatv("\nFailed to open FFI output file {0}: {1}", ffi_file_path, ec.message()));
  }

  llvm::raw_fd_ostream makefile_out(makefile_path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    return CreateRuntimeError(
        llvm::formatv("\nFailed to open Makefile output file {0}: {1}", makefile_path, ec.message()));
  }

  tu_out << tu_str;
  tu_out.flush();
  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Wrote pancake translation unit to " << tu_file_path << "\n";

  scaffold_out << llvm::formatv(SCAFFOLD_TEMPLATE, heap_size + stack_size, heap_size, stack_size);
  scaffold_out.flush();
  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Wrote pancake scaffold to " << scaffold_file_path << "\n";

  ffi_out << llvm::formatv(FFI_TEMPLATE, ffi_str);
  ffi_out.flush();
  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Wrote pancake FFI to " << ffi_file_path << "\n";

  makefile_out << llvm::formatv(MAKEFILE_TEMPLATE, cake_path, cake_options, ctx.next_file);
  makefile_out.flush();
  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Wrote pancake Makefile to " << makefile_path << "\n";

  // see https://github.com/CakeML/cakeml/blob/pan_howto/pancake/how-to.md
  // run cake --pancake < name.pancake > name.S

  auto real_make_path = llvm::sys::findProgramByName(make_path);
  if (!real_make_path) {
    return CreateRuntimeError(
        llvm::formatv("\nFailed to find 'make' in PATH: {0}", real_make_path.getError().message()));
  }

  std::optional<llvm::sys::ProcessStatistics> stats = std::nullopt;
  std::string error_message;
  bool execution_failed = true;
  auto exit_code = llvm::sys::ExecuteAndWait(real_make_path.get(),
                                             {
                                                 real_make_path.get(),
                                                 "-f",
                                                 makefile_path,
                                                 "all",
                                             },
                                             std::nullopt, {}, 0, 0, &error_message, &execution_failed, &stats);

  if (execution_failed || exit_code != 0) {
    llvm::SmallString<64> pwd;
    return CreateRuntimeError(llvm::formatv("\nFailed to execute make ({0}): {1}\n\ncake command: cpp "
                                            "-P < {2} | cake --pancake --main_return=true "
                                            "> {3}\nmake all command: {4} -f {5} all",
                                            exit_code, error_message, tu_file_path, asm_path, real_make_path.get(),
                                            makefile_path));
  }

  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Executed make successfully, total time: "
               << (stats ? stats->TotalTime : std::chrono::microseconds(0)).count()
               << " us, user time: " << (stats ? stats->UserTime : std::chrono::microseconds(0)).count()
               << " us, peak memory: " << (stats ? stats->PeakMemory : 0) << " KiB\n";

  auto error = llvm::sys::fs::setPermissions(
      bin_path, llvm::sys::fs::perms::owner_all | llvm::sys::fs::perms::group_read | llvm::sys::fs::perms::group_exe |
                    llvm::sys::fs::perms::others_read | llvm::sys::fs::perms::others_exe);
  if (error) {
    return CreateRuntimeError(
        llvm::formatv("\nFailed to set permissions on binary {0}: {1}", bin_path, error.message()));
  }

  PrintLogBegin(llvm::outs(), ctx);
  llvm::outs() << "Binary at " << bin_path << "\n";

  return llvm::Error::success();
}
} // namespace pancake::pass_c2pancake
