#include "Pass_FunctionCalling.h"
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
#include <llvm/ADT/ScopeExit.h>
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

namespace pancake::pass_function_calling {
namespace CollectFunctionInfo {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;

  struct ReturnInfo {
    QualType return_type; // COULD be void
    std::optional<llvm::SmallString<32>> hoisted_name;
    explicit ReturnInfo(QualType return_type, std::optional<llvm::SmallString<32>> hoisted_name = std::nullopt)
        : return_type(return_type), hoisted_name(std::move(hoisted_name)) {}
  };
  struct ParamInfo {
    ParmVarDecl *param_decl; // type COULD be void
    std::optional<llvm::SmallString<32>> hoisted_name;
    explicit ParamInfo(ParmVarDecl *param_decl, std::optional<llvm::SmallString<32>> hoisted_name = std::nullopt)
        : param_decl(param_decl), hoisted_name(std::move(hoisted_name)) {}
  };
  struct FunctionInfo {
    std::optional<ReturnInfo> return_info;
    llvm::SmallVector<ParamInfo, 16> param_infos;
    explicit FunctionInfo(std::optional<ReturnInfo> return_info, llvm::SmallVector<ParamInfo, 16> param_infos)
        : return_info(std::move(return_info)), param_infos(std::move(param_infos)) {}
  };
  llvm::DenseMap<FunctionDecl *, FunctionInfo> &function_map;

  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return false; }

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.pa_ctx.major_pass_number, data.pa_ctx.minor_pass_number,
                         data.tmp_var_counter++);
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(func_decl->getBeginLoc())) || !func_decl->isThisDeclarationADefinition()) {
      return false;
    }

    auto it = data.function_map.find(func_decl);
    if (it == data.function_map.end()) {
      // process the function decl
      auto return_type = func_decl->getReturnType();
      if ((return_type->isIntegerType() || return_type->isPointerType()) &&
          data.Ctx.getTypeSize(return_type) > GetPointerWidth(data.Ctx)) {
        data.error = CreateRuntimeError(llvm::formatv(
            "FunctionDecl {0} has integer return type {1} larger than pointer width at {2}", func_decl->getName(),
            return_type.getAsString(), func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager())));
        return false;
      }

      auto return_info = WorkerData::ReturnInfo(return_type);
      if (!return_type->isIntegerType() && !return_type->isPointerType() && !return_type->isVoidType()) {
        // hoist the return type
        auto hoisted_name = llvm::SmallString<32>(llvm::formatv("{0}_return", func_decl->getName()));
        return_info.hoisted_name = std::move(hoisted_name);
      }

      llvm::SmallVector<WorkerData::ParamInfo, 16> param_infos;
      size_t inline_params_count = 0;
      for (const auto [i, param_decl] : std::views::enumerate(func_decl->parameters())) {
        auto param_type = param_decl->getType();
        if ((param_type->isIntegerType() || param_type->isPointerType()) &&
            data.Ctx.getTypeSize(param_type) > GetPointerWidth(data.Ctx)) {
          data.error = CreateRuntimeError(
              llvm::formatv("FunctionDecl {0} has integer parameter {1} of type {2} larger than pointer width at {3}",
                            func_decl->getName(), param_decl->getName(), param_type.getAsString(),
                            param_decl->getBeginLoc().printToString(data.Ctx.getSourceManager())));
          return false;
        }

        auto param_info = WorkerData::ParamInfo(param_decl);
        std::optional<llvm::SmallString<32>> hoisted_name = std::nullopt;
        if (!param_type->isIntegerType() && !param_type->isPointerType() && !param_type->isVoidType()) {
          // hoist the parameter type
          hoisted_name =
              llvm::SmallString<32>(llvm::formatv("{0}_param{1}_{2}", func_decl->getName(), i, param_decl->getName()));
        } else {
          // pancake only supports 4 inline parameters, so if we have more than 4, we need to hoist the parameter type
          if (inline_params_count >= 4) {
            hoisted_name = llvm::SmallString<32>(
                llvm::formatv("{0}_param{1}_{2}", func_decl->getName(), i, param_decl->getName()));
          } else {
            inline_params_count++;
          }
        }
        param_info.hoisted_name = std::move(hoisted_name);
        param_infos.push_back(std::move(param_info));
      }

      data.function_map.insert({func_decl, WorkerData::FunctionInfo(std::move(return_info), std::move(param_infos))});
    }

    return true;
  }
};
} // namespace
} // namespace CollectFunctionInfo

namespace RewriteFunctions {
namespace {
namespace {
using FunctionInfo = CollectFunctionInfo::WorkerData::FunctionInfo;

struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  llvm::DenseMap<FunctionDecl *, FunctionInfo> &function_map;

  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return false; }

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

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(func_decl->getBeginLoc())) || !func_decl->isThisDeclarationADefinition()) {
      return false;
    }

    auto it = data.function_map.find(func_decl);
    if (it == data.function_map.end()) {
      data.error =
          CreateRuntimeError(llvm::formatv("FunctionDecl {0} not found in function_map at {1}", func_decl->getName(),
                                           func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager())));
      return false;
    }

    auto &function_info = it->second;

    if (func_decl->isThisDeclarationADefinition()) {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);

      // add global vars for hoisted return type and parameters
      os << "\n/* c2pancake: start hoisted return type and parameters for function " << func_decl->getName() << " */\n";
      for (const auto [i, param_info] : std::views::enumerate(function_info.param_infos)) {
        if (param_info.hoisted_name.has_value()) {
          os << PrintType(param_info.param_decl->getType(), param_info.hoisted_name.value()) << ";\n";
        }
      }
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        if (return_info.hoisted_name.has_value()) {
          os << PrintType(return_info.return_type, return_info.hoisted_name.value()) << ";\n";
        }
      }
      os << "/* c2pancake: end hoisted return type and parameters for function " << func_decl->getName() << " */\n";

      // now do the function signature

      // return type
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        os << return_info.return_type.getAsString();
      } else {
        os << "void ";
      }

      // name
      os << func_decl->getName();

      // args
      os << "(";
      for (const auto [i, param_info] : std::views::enumerate(function_info.param_infos)) {
        if (i > 0) {
          os << ", ";
        }
        if (!param_info.hoisted_name.has_value()) {
          os << PrintType(param_info.param_decl->getType(), param_info.param_decl->getName());
        }
      }
      os << ")";

      // now do the function prologue to load all the hoisted parameters into local variables
      os << " {\n";
      os << "/* c2pancake: start function prologue for function " << func_decl->getName() << " */\n";
      for (const auto [i, param_info] : std::views::enumerate(function_info.param_infos)) {
        if (param_info.hoisted_name.has_value()) {
          os << llvm::formatv("{0} = {1};\n",
                              PrintType(param_info.param_decl->getType(), param_info.param_decl->getName()),
                              param_info.hoisted_name.value());
        }
      }
      os << "/* c2pancake: end function prologue for function " << func_decl->getName() << " */\n";

    } else {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        os << return_info.return_type.getAsString();
      } else {
        os << "void ";
      }

      os << func_decl->getName() << "(";
      for (const auto [i, param_info] : std::views::enumerate(function_info.param_infos)) {
        if (i > 0) {
          os << ", ";
        }
        if (!param_info.hoisted_name.has_value()) {
          os << PrintType(param_info.param_decl->getType(), param_info.param_decl->getName());
        }
      }
      os << ");";
    }

    return true;
  }

  auto TraverseCallExpr(CallExpr *call_expr) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(call_expr->getBeginLoc()))) {
      return true;
    }

    auto *callee_decl = call_expr->getDirectCallee();
    if (callee_decl == nullptr) {
      // maybe function pointer???
      data.error = CreateRuntimeError(llvm::formatv(
          "CallExpr at {0} has no direct callee", call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager())));
      return false;
    }

    auto it = data.function_map.find(callee_decl);
    if (it != data.function_map.end()) {
      auto &function_info = it->second;
    }

    return true;
  };
};
} // namespace
} // namespace RewriteFunctions

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::DenseMap<FunctionDecl *, FunctionInfo> function_map;
  {
    CollectFunctionInfo::WorkerData data{.Ctx = Ctx, .pa_ctx = ps_ctx, .function_map = function_map};
    CollectFunctionInfo::Worker w(data);
    w.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (data.error) {
      ps_ctx.error = std::move(data.error);
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  llvm::SmallVector<Replacement, 64> replacements;
  {
    RewriteFunctions::WorkerData data{
        .Ctx = Ctx, .pa_ctx = ps_ctx, .replacements = replacements, .function_map = function_map};
    RewriteFunctions::Worker w(data);
    w.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (data.error) {
      ps_ctx.error = std::move(data.error);
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }

    for (const auto &r : replacements) {
      if (auto err = ps_ctx.replacements.add(r)) {
        ps_ctx.error = CreateRuntimeError(llvm::formatv("Add replacement conflict: {0}", err));
        ps_ctx.whats_next = WhatsNext::MoveToNextFile;
        return;
      }
    }
  }

  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_promote_records
