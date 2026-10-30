#include "Pass_C2Pancake.h"
#include "CodeGen.h"
#include "Utils.h"
#include "pancake_ir.h"

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

#include <functional>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;

extern llvm::cl::opt<uint64_t> stack_size_opt;
extern llvm::cl::opt<std::string> make_path;
extern llvm::cl::opt<std::string> cake_path;
extern llvm::cl::opt<std::string> cake_options;

namespace pancake::pass_c2pancake {
using clang::BreakStmt;
using clang::CallExpr;
using clang::ContinueStmt;
using clang::Expr;
using clang::IfStmt;
using clang::ReturnStmt;
using clang::Stmt;
using clang::WhileStmt;
namespace {
struct WorkerData {
  ASTContext &Ctx;
  StageContext &ps_ctx;
  llvm::raw_ostream &ffi_os;
  llvm::DenseMap<VarDecl *, GlobalVarData> &global_var_map;
  pancake::Program program;
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
  std::string final_expr_text;
  std::string final_expr;
  pancake::ExprPtr expr;
  std::optional<size_t> heap_alignment;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type, std::optional<size_t> heap_alignment)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr_text(final_expr),
        final_expr(final_expr_text), expr(std::make_shared<pancake::RawExpr>(std::move(final_expr))),
        heap_alignment(heap_alignment) {}
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  static auto IsVolatileAccess(const Expr *expr) -> bool {
    expr = expr->IgnoreParenImpCasts();
    if (expr->getType().isVolatileQualified()) {
      return true;
    }
    if (const auto *member_expr = dyn_cast<MemberExpr>(expr)) {
      return IsVolatileAccess(member_expr->getBase());
    }
    if (const auto *unary_operator = dyn_cast<UnaryOperator>(expr);
        unary_operator != nullptr && unary_operator->getOpcode() == UO_Deref) {
      const auto *pointer_type = unary_operator->getSubExpr()->getType()->getAs<PointerType>();
      return pointer_type != nullptr && pointer_type->getPointeeType().isVolatileQualified();
    }
    return false;
  }

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return true; }

  auto TraverseTranslationUnitDecl(TranslationUnitDecl *tu_decl) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = tu_decl->getASTContext().getSourceManager();

    for (auto *decl : tu_decl->decls()) {
      // inline stuff from headers will appear here but we want to filter all of them out
      auto loc = decl->getLocation();
      if (loc.isInvalid() || !sm.isWrittenInMainFile(loc)) {
        continue;
      }

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

        std::string pointee_type_str = data.Ctx.getPointerType(var_decl->getType()).getAsString();
        data.global_var_map[var_decl] = {.type = pointee_type_str,
                                         .name = var_decl->getName().str(),
                                         .addr = addr,
                                         .is_external = IsGlobalVarExternal(var_decl)};

        data.program.globals.emplace_back(std::make_shared<pancake::DefineStmt>(
            var_decl->getName().str() + "_addr", llvm::formatv("@base + {0}", addr).str()));

        std::string value_str;
        llvm::raw_string_ostream value_os(value_str);
        value_os << "(";
        switch (size_bytes) {
        case 1:
          value_os << "ld8";
          break;
        case 4:
          value_os << "ld32";
          break;
        case 8:
          value_os << "lds 1";
          break;
        default:
          data.error = CreateRuntimeError(std::move(llvm::formatv(
              "\n    at {0}\nUnsupported global variable {1} size: {2} bytes",
              var_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), var_decl->getName(), size_bytes)));
          return false;
        }
        value_os << " (@base + " << addr << ")";
        value_os << ")";
        value_os.flush();
        data.program.globals.emplace_back(
            std::make_shared<pancake::DefineStmt>(var_decl->getName().str() + "_val", value_str));
        continue;
      }

      // apparently these can also appear starting C11?!
      if (auto *_ = llvm::dyn_cast<StaticAssertDecl>(decl)) {
        continue;
      }

      // I didn't even know this existed.......... :skull:
      if (auto *_ = llvm::dyn_cast<EmptyDecl>(decl)) {
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

  static auto IsGlobalVarExternal(VarDecl *var_decl) -> bool {
    return std::ranges::any_of(var_decl->getAttrs(), [](const Attr *attr) -> bool {
      if (const auto *annotate_attr = llvm::dyn_cast<AnnotateAttr>(attr)) {
        if (annotate_attr->getAnnotation() == "__c2pnk_external_global") {
          return true;
        }
      }
      return false;
    });
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
        llvm::SmallVector<pancake::StmtPtr> stmts;
        stmts.reserve(compound_stmt->size());
        std::function<void(const pancake::StmtPtr &)> append_statement = [&](const pancake::StmtPtr &statement) {
          if (statement == nullptr)
            return;
          if (statement->kind == pancake::StmtKind::Block) {
            const auto &nested = static_cast<const pancake::BlockStmt &>(*statement);
            for (const auto &child : nested.block->stmts)
              append_statement(child);
            return;
          }
          stmts.emplace_back(statement);
        };
        for (auto *stmt : compound_stmt->body()) {
          auto res = BuildAstStmt(stmt);
          if (auto error = res.takeError()) {
            data.error = std::move(error);
            return false;
          }
          append_statement(res.get());
        }

        auto function = std::make_shared<pancake::Function>();
        function->name = func_decl->getName().str();
        function->isInline = func_decl->isInlineSpecified();
        function->body = std::make_shared<pancake::Block>();
        for (auto &&param : func_decl->parameters()) {
          function->params.push_back({param->getName().str(), 1});
        }
        for (auto &&stmt : stmts)
          function->body->stmts.emplace_back(std::move(stmt));
        data.program.functions.emplace_back(std::move(function));
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

  auto ParseAstStmt(std::string text) -> Expected<pancake::StmtPtr> {
    auto trim = [](std::string value) {
      const auto begin = value.find_first_not_of(" \t\n");
      if (begin == std::string::npos)
        return std::string{};
      const auto end = value.find_last_not_of(" \t\n;");
      return value.substr(begin, end - begin + 1);
    };
    text = trim(std::move(text));
    if (text.empty())
      return pancake::StmtPtr{};
    if (text.starts_with("!ld")) {
      const auto opcode_end = text.find(' ');
      const auto comma = text.find(", ", opcode_end == std::string::npos ? 0 : opcode_end + 1);
      if (opcode_end == std::string::npos || comma == std::string::npos)
        return CreateRuntimeError("Malformed shared memory load");
      const auto opcode = text.substr(1, opcode_end - 1);
      pancake::MemoryWidth width;
      if (opcode == "ldw")
        width = pancake::MemoryWidth::Word;
      else if (opcode == "ld8")
        width = pancake::MemoryWidth::Byte;
      else if (opcode == "ld32")
        width = pancake::MemoryWidth::Word32;
      else
        return CreateRuntimeError(llvm::formatv("Unsupported shared memory load: {0}", opcode));
      return std::make_shared<pancake::SharedMemoryLoadStmt>(
          width, text.substr(opcode_end + 1, comma - opcode_end - 1),
          std::make_shared<pancake::RawExpr>(text.substr(comma + 2)));
    }
    if (text == "break")
      return std::make_shared<pancake::BreakStmt>();
    if (text.starts_with("return "))
      return std::make_shared<pancake::ReturnStmt>(std::make_shared<pancake::RawExpr>(text.substr(7)));
    if (text.starts_with("var ")) {
      auto name_start = text.find(' ', 4);
      if (name_start == std::string::npos)
        return CreateRuntimeError("Malformed lowered variable declaration");
      ++name_start;
      auto equals = text.find(" = ", name_start);
      auto name = text.substr(name_start, equals == std::string::npos ? std::string::npos : equals - name_start);
      pancake::ExprPtr init;
      if (equals != std::string::npos)
        init = std::make_shared<pancake::RawExpr>(text.substr(equals + 3));
      return std::make_shared<pancake::VarDeclStmt>(std::move(name), std::move(init), 1);
    }
    const auto equals = text.find(" = ");
    if (equals != std::string::npos)
      return std::make_shared<pancake::AssignStmt>(std::make_shared<pancake::RawExpr>(text.substr(0, equals)),
                                                   std::make_shared<pancake::RawExpr>(text.substr(equals + 3)));
    return CreateRuntimeError(llvm::formatv("Unsupported lowered statement: {0}", text));
  }

  auto BuildAstStmt(Stmt *stmt) -> Expected<pancake::StmtPtr> {
    auto AppendToBlock = [](const pancake::StmtPtr &statement, const pancake::BlockPtr &block) {
      if (statement == nullptr)
        return;
      if (statement->kind == pancake::StmtKind::Block) {
        const auto &nested = static_cast<const pancake::BlockStmt &>(*statement);
        for (const auto &child : nested.block->stmts) {
          if (child != nullptr && child->kind == pancake::StmtKind::Block) {
            const auto &nested_child = static_cast<const pancake::BlockStmt &>(*child);
            for (const auto &grandchild : nested_child.block->stmts)
              block->stmts.emplace_back(grandchild);
          } else {
            block->stmts.emplace_back(child);
          }
        }
        return;
      }
      block->stmts.emplace_back(statement);
    };
    auto WithPreStatements = [&](const llvm::SmallVector<std::string, 8> &pre_stmts,
                                 pancake::StmtPtr statement) -> Expected<pancake::StmtPtr> {
      if (pre_stmts.empty())
        return statement;
      auto block = std::make_shared<pancake::Block>();
      for (const auto &pre_stmt : pre_stmts) {
        auto pre = ParseAstStmt(pre_stmt);
        if (auto error = pre.takeError())
          return std::move(error);
        if (pre.get())
          block->stmts.emplace_back(std::move(pre.get()));
      }
      if (statement)
        block->stmts.emplace_back(std::move(statement));
      if (block->stmts.empty())
        return pancake::StmtPtr{};
      return std::make_shared<pancake::BlockStmt>(std::move(block));
    };

    if (auto *compound = dyn_cast<CompoundStmt>(stmt)) {
      auto block = std::make_shared<pancake::Block>();
      for (auto *child : compound->body()) {
        auto result = BuildAstStmt(child);
        if (auto error = result.takeError())
          return std::move(error);
        AppendToBlock(result.get(), block);
      }
      return std::make_shared<pancake::BlockStmt>(std::move(block));
    }
    if (auto *if_stmt = dyn_cast<IfStmt>(stmt)) {
      auto condition = BuildExpr(BuildExprCtx(if_stmt->getCond(), Usage::Value));
      if (auto error = condition.takeError())
        return std::move(error);
      auto then_result = BuildAstStmt(if_stmt->getThen());
      if (auto error = then_result.takeError())
        return std::move(error);
      auto then_block = std::make_shared<pancake::Block>();
      AppendToBlock(then_result.get(), then_block);
      pancake::BlockPtr else_block;
      if (auto *else_stmt = if_stmt->getElse()) {
        auto else_result = BuildAstStmt(else_stmt);
        if (auto error = else_result.takeError())
          return std::move(error);
        else_block = std::make_shared<pancake::Block>();
        AppendToBlock(else_result.get(), else_block);
      }
      return std::make_shared<pancake::IfStmt>(std::move(condition->expr), std::move(then_block),
                                               std::move(else_block));
    }
    if (auto *while_stmt = dyn_cast<WhileStmt>(stmt)) {
      auto condition = BuildExpr(BuildExprCtx(while_stmt->getCond(), Usage::Value));
      if (auto error = condition.takeError())
        return std::move(error);
      auto body_result = BuildAstStmt(while_stmt->getBody());
      if (auto error = body_result.takeError())
        return std::move(error);
      auto body = std::make_shared<pancake::Block>();
      AppendToBlock(body_result.get(), body);
      return std::make_shared<pancake::WhileStmt>(std::move(condition->expr), std::move(body));
    }
    if (isa<BreakStmt>(stmt))
      return std::make_shared<pancake::BreakStmt>();
    if (isa<ContinueStmt>(stmt))
      return std::make_shared<pancake::ContinueStmt>();

    if (auto *decl_stmt = dyn_cast<DeclStmt>(stmt)) {
      auto block = std::make_shared<pancake::Block>();
      for (auto *decl : decl_stmt->decls()) {
        auto *var_decl = dyn_cast<VarDecl>(decl);
        if (var_decl == nullptr)
          return CreateRuntimeError("Unsupported declaration in Pancake AST conversion");
        BuiltExpr *init_result = nullptr;
        std::optional<BuiltExpr> owned_init;
        if (var_decl->hasInit() && !var_decl->getInit()->getType()->isArrayType()) {
          auto result = BuildExpr(BuildExprCtx(var_decl->getInit(), Usage::Value));
          if (auto error = result.takeError())
            return std::move(error);
          owned_init.emplace(std::move(result.get()));
          init_result = &*owned_init;
          for (const auto &pre_stmt : init_result->pre_stmts) {
            auto pre = ParseAstStmt(pre_stmt);
            if (auto error = pre.takeError())
              return std::move(error);
            if (pre.get())
              block->stmts.emplace_back(std::move(pre.get()));
          }
        }
        pancake::ExprPtr init =
            init_result == nullptr ? std::make_shared<pancake::IntLitExpr>(0) : std::move(init_result->expr);
        block->stmts.emplace_back(
            std::make_shared<pancake::VarDeclStmt>(var_decl->getName().str(), std::move(init), 1));
      }
      if (block->stmts.size() == 1)
        return std::move(block->stmts.front());
      return std::make_shared<pancake::BlockStmt>(std::move(block));
    }

    if (auto *return_stmt = dyn_cast<ReturnStmt>(stmt)) {
      if (return_stmt->getRetValue() == nullptr)
        return std::make_shared<pancake::ReturnStmt>();
      auto result = BuildExpr(BuildExprCtx(return_stmt->getRetValue(), Usage::Value));
      if (auto error = result.takeError())
        return std::move(error);
      return WithPreStatements(result->pre_stmts, std::make_shared<pancake::ReturnStmt>(std::move(result->expr)));
    }

    if (auto *expr_stmt = dyn_cast<clang::Expr>(stmt)) {
      if (!expr_stmt->HasSideEffects(data.Ctx))
        return pancake::StmtPtr{};
      auto result = BuildExpr(BuildExprCtx(expr_stmt, Usage::Effect));
      if (auto error = result.takeError())
        return std::move(error);
      if (result->expr == nullptr || result->final_expr.empty())
        return WithPreStatements(result->pre_stmts, pancake::StmtPtr{});
      return WithPreStatements(result->pre_stmts, std::make_shared<pancake::ExprStmt>(std::move(result->expr)));
    }

    auto lowered = BuildStmt(stmt);
    if (auto error = lowered.takeError())
      return std::move(error);
    return ParseAstStmt(std::move(lowered.get()));
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
            if (IsVolatileAccess(init_expr) && !init_expr->getType()->isArrayType()) {
              auto address_res = BuildExpr(BuildExprCtx(init_expr, Usage::Place));
              if (auto error = address_res.takeError()) {
                return std::move(error);
              }

              const auto size_bytes = (uint64_t)data.Ctx.getTypeSizeInChars(init_expr->getType()).getQuantity();
              std::string load_op;
              switch (size_bytes) {
              case 1:
                load_op = "ld8";
                break;
              case 4:
                load_op = "ld32";
                break;
              case 8:
                load_op = "lds 1";
                break;
              default:
                return CreateRuntimeError(std::move(
                    llvm::formatv("\n    at {0}\nUnsupported volatile initializer size: {1} bytes",
                                  init_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), size_bytes)));
              }

              for (auto &&pre_stmt : address_res->pre_stmts) {
                os << pre_stmt << "\n";
              }
              auto shared_load_op = size_bytes == 1 ? "ld8" : size_bytes == 4 ? "ld32" : "ldw";
              os << llvm::formatv("var 1 {0} = 0;\n!{1} {0}, {2};", var_decl->getName(), shared_load_op,
                                  address_res->final_expr);
              continue;
            }

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
          // auto addr = data.global_var_map[var_decl].addr;
          // auto bytes = (uint64_t)data.Ctx.getTypeSizeInChars(var_decl->getType()).getQuantity();
          auto usage_kind = ctx.usage_kind;
          // if type is array, just decay to pointer AKA place
          if (final_expr_type->isArrayType()) {
            usage_kind = Usage::Place;
          }

          auto name = llvm::formatv("{0}_{1}", var_decl->getName(), Usage::Value == usage_kind ? "val" : "addr");
          return BuiltExpr(std::move(pre_stmts), name.str(), final_expr_type, std::nullopt);
        }

        // local var
        os << var_decl->getName();
        os.flush();
        return BuiltExpr(std::move(pre_stmts), final_expr, final_expr_type, std::nullopt);
      }

      if (auto *enum_constant_decl = dyn_cast<EnumConstantDecl>(decl_ref_expr->getDecl())) {
        auto value = enum_constant_decl->getInitVal();
        os << llvm::formatv("{0} /* {1}", value.getZExtValue(), enum_constant_decl->getName());
        if (value.getZExtValue() > 15) {
          os << llvm::formatv(" ({0:X})", value.getZExtValue());
        }
        os << " */";
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
      if (integer_literal->getValue().getZExtValue() > 15) {
        os << llvm::formatv(" /* {0:X} */", integer_literal->getValue().getZExtValue());
      }
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

      os << " /* '";
      char c = static_cast<char>(character_literal->getValue());
      llvm::printEscapedString(llvm::StringRef(&c, 1), os);
      os << "' */";

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
          // check if volatile...
          if (lhs->getType().isVolatileQualified())
            os << "!";

          switch (heap_alignment.value()) {
          case 1: {
            os << "st8";
            break;
          }
          case 4: {
            os << "st32";
            break;
          }
          case 8: {
            os << "st";
          } break;
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
            heap_alignment = (size_t)data.Ctx.getTypeAlignInChars(sub_expr->getType()->getPointeeType()).getQuantity();
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
          std::string load_op;
          switch (size_bytes) {
          case 1:
            load_op = "ld8";
            break;
          case 4:
            load_op = "ld32";
            break;
          case 8:
            load_op = "lds 1";
            break;
          default:
            return CreateRuntimeError(std::move(
                llvm::formatv("\n    at {0}\nUnsupported dereferenced type size: {1} bytes",
                              unary_operator->getExprLoc().printToString(data.Ctx.getSourceManager()), size_bytes)));
          }
          pre_stmts.insert(pre_stmts.end(), sub_expr_res->pre_stmts.begin(), sub_expr_res->pre_stmts.end());
          if (IsVolatileAccess(unary_operator)) {
            auto deref_tmp = llvm::formatv("__c2pnk_VolatileLoad_{0}", data.tmp_var_counter++).str();
            auto shared_load_op = size_bytes == 1 ? "ld8" : size_bytes == 4 ? "ld32" : "ldw";
            pre_stmts.push_back(llvm::formatv("var 1 {0} = 0;", deref_tmp).str());
            pre_stmts.push_back(
                llvm::formatv("!{0} {1}, {2};", shared_load_op, deref_tmp, sub_expr_res->final_expr).str());
            os << deref_tmp;
            break;
          }
          // Materialize loads before they are used in another load or address expression. Nested lds expressions are
          // not handled reliably by the Pancake compiler.
          auto load_expr = llvm::formatv("{0} ({1})", load_op, sub_expr_res->final_expr).str();
          auto deref_tmp = llvm::formatv("__c2pnk_deref_tmp_{0}", data.tmp_var_counter++).str();
          pre_stmts.push_back(llvm::formatv("var 1 {0} = {1};", deref_tmp, load_expr).str());
          final_expr.clear();
          os << deref_tmp;
          break;
        }

        case Usage::Place: {
          // this branch happens if you have something like *p = 3; where p is a pointer to a variable
          // the *p is the expr here and the usage kind is place because it's the LHS of an assignment
          // we basically treat the *p as &p - funny how that works out

          // The place of *p is the address held by p, so p itself must be
          // evaluated as a value rather than as the address of its storage.
          auto sub_expr_res = BuildExpr(BuildExprCtx(sub_expr, Usage::Value));
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

        // Keep the complete address together when the base is a computed pointer load; otherwise `st` can parse the
        // load and offset as separate operands.
        auto member_address = llvm::formatv("({0} + {1})", member_base_expr->final_expr, offset_in_bytes).str();
        if (ctx.usage_kind == Usage::Value && !final_expr_type->isArrayType()) {
          const auto member_size = (uint64_t)data.Ctx.getTypeSizeInChars(final_expr_type).getQuantity();
          std::string load_op;
          switch (member_size) {
          case 1:
            load_op = "ld8";
            break;
          case 4:
            load_op = "ld32";
            break;
          case 8:
            load_op = "lds 1";
            break;
          default:
            return CreateRuntimeError(std::move(
                llvm::formatv("\n    at {0}\nUnsupported member value size: {1} bytes",
                              member_expr->getExprLoc().printToString(data.Ctx.getSourceManager()), member_size)));
          }
          if (IsVolatileAccess(member_expr)) {
            auto member_tmp = llvm::formatv("__c2pnk_VolatileLoad_{0}", data.tmp_var_counter++).str();
            auto shared_load_op = member_size == 1 ? "ld8" : member_size == 4 ? "ld32" : "ldw";
            pre_stmts.push_back(llvm::formatv("var 1 {0} = 0;", member_tmp).str());
            pre_stmts.push_back(llvm::formatv("!{0} {1}, {2};", shared_load_op, member_tmp, member_address).str());
            os << member_tmp;
          } else {
            os << llvm::formatv("({0} {1})", load_op, member_address);
          }
        } else {
          os << member_address;
        }
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
  std::string ffi_replacement_text;
  llvm::raw_string_ostream ffi_os(ffi_replacement_text);

  llvm::DenseMap<VarDecl *, GlobalVarData> global_var_map;
  WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .ffi_os = ffi_os, .global_var_map = global_var_map};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  ffi_os.flush();

  if (data.error) {
    ps_ctx.error = std::move(data.error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }

  pancake::CodeGen codegen;
  auto tu_output = codegen.Generate(data.program);
  if (auto error = ps_ctx.run.artifacts.Set(translation_unit_output_key,
                                            TranslationUnitOutput{.content = std::move(tu_output)})) {
    ps_ctx.error = std::move(error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }
  if (auto error = ps_ctx.run.artifacts.Set(ffi_output_key, FFIOutput{.content = std::move(ffi_replacement_text)})) {
    ps_ctx.error = std::move(error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }
  if (auto error = ps_ctx.run.artifacts.Set(global_var_data_key, std::move(global_var_map))) {
    ps_ctx.error = std::move(error);
    ps_ctx.SetControl(StageControl::NextFile);
    return;
  }
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

  /* externally declared global vars go here, they are all POINTERS */
  {3}
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
  const auto &external_global_vars_map = ctx.run.artifacts.Get(global_var_data_key);
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

  std::string external_global_vars;
  llvm::raw_string_ostream external_global_vars_os(external_global_vars);
  for (const auto &[var_decl, global_var_data] : *external_global_vars_map) {
    /* note that {1} is a POINTER to the original type! */
    external_global_vars_os << llvm::formatv("*({1})(((unsigned char *)cml_heap) + {0}) = 0; /* {2}*/\n",
                                             global_var_data.addr, global_var_data.type, global_var_data.name);
  }
  external_global_vars_os.flush();

  scaffold_out << llvm::formatv(SCAFFOLD_TEMPLATE, heap_size + stack_size, heap_size, stack_size, external_global_vars);
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
