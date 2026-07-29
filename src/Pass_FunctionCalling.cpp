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

namespace pancake::pass_inject_memcpy_polyfill {
namespace {
const char *memcpy_polyfill_code = R"(
#include <stdint.h>
static inline uint{0}_t __cp2nk_memcpy(uint8_t *dest, uint8_t *src, uint{0}_t len) {{
  while (len > 0UL) {{
    *dest = *src;
    dest = dest + 1UL;
    src = src + 1UL;
    len = len - 1UL;
  }
  return 0UL;
}
)";
}

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  const auto &sm = Ctx.getSourceManager();
  if (auto err = ps_ctx.replacements.add({sm, sm.getLocForStartOfFile(sm.getMainFileID()), 0, memcpy_polyfill_code})) {
    ps_ctx.error = CreateRuntimeError(llvm::formatv("Add replacement conflict: {0}", err));
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }
  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_inject_memcpy_polyfill

namespace pancake::pass_function_calling {
namespace {
using NonFFIFunctionDefRewrites = llvm::DenseMap<FunctionDecl *, Replacement>; // non-ffi function definition rewrites.
using NonFFIFunctionDefReturnRewrites =
    llvm::DenseMap<FunctionDecl *, Replacements>; // non-ffi function definition return rewrites.
using NonFFIFunctionDeclRewrites =
    llvm::DenseMap<FunctionDecl *, Replacements>; // non-ffi function declaration rewrites.
using NonFFIFunctionCallRewrites = llvm::DenseMap<FunctionDecl *, Replacements>; // non-ffi function call rewrites.

using FFIFunctionWrapperRewrites =
    llvm::DenseMap<FunctionDecl *, Replacement>; // ffi function wrapper rewrites. should be 1 per FFI function. They
                                                 // are appended to the top of the source file.
using FFIFunctionCallRewrites = llvm::DenseMap<FunctionDecl *, Replacements>; // ffi function call rewrites. should be 1
                                                                              // per FFI function call site.

namespace CollectFunctionInfo {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;

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
    bool needs_rewriting = false;
    explicit FunctionInfo(std::optional<ReturnInfo> return_info, llvm::SmallVector<ParamInfo, 16> param_infos,
                          bool needs_rewriting)
        : return_info(std::move(return_info)), param_infos(std::move(param_infos)), needs_rewriting(needs_rewriting) {}
  };
  using FunctionMap = llvm::DenseMap<FunctionDecl *, FunctionInfo>;
  FunctionMap &function_map;

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
      return true;
    }

    if (func_decl->isVariadic()) {
      data.error = CreateRuntimeError(
          llvm::formatv("\n    at {0}\nNon-FFI FunctionDecl {1} is variadic, which is not supported by pancake",
                        func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
      return false;
    }

    auto it = data.function_map.find(func_decl);
    if (it == data.function_map.end()) {
      // process the function decl
      auto return_type = func_decl->getReturnType();
      if ((return_type->isIntegerType() || return_type->isPointerType()) &&
          data.Ctx.getTypeSize(return_type) > GetPointerWidth(data.Ctx)) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nFunctionDecl {1} has integer return type {2} larger than pointer width",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName(),
                          return_type.getAsString()));
        return false;
      }

      bool needs_rewriting = false;
      auto return_info = WorkerData::ReturnInfo(return_type);
      if (!return_type->isIntegerType() && !return_type->isPointerType() && !return_type->isVoidType()) {
        // hoist the return type
        auto hoisted_name = llvm::SmallString<32>(llvm::formatv("{0}_return", func_decl->getName()));
        return_info.hoisted_name = std::move(hoisted_name);
        needs_rewriting = true;
      }

      llvm::SmallVector<WorkerData::ParamInfo, 16> param_infos;
      for (const auto [i, param_decl] : std::views::enumerate(func_decl->parameters())) {
        auto param_type = param_decl->getType();
        if ((param_type->isIntegerType() || param_type->isPointerType()) &&
            data.Ctx.getTypeSize(param_type) > GetPointerWidth(data.Ctx)) {
          data.error = CreateRuntimeError(llvm::formatv(
              "\n    at {0}\nFunctionDecl {1} has integer parameter {2} of type {3} larger than pointer width",
              param_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName(),
              param_decl->getName(), param_type.getAsString()));
          return false;
        }

        auto param_info = WorkerData::ParamInfo(param_decl);
        std::optional<llvm::SmallString<32>> hoisted_name = std::nullopt;
        if (!param_type->isIntegerType() && !param_type->isPointerType() && !param_type->isVoidType()) {
          // hoist the parameter type
          hoisted_name =
              llvm::SmallString<32>(llvm::formatv("{0}_param{1}_{2}", func_decl->getName(), i, param_decl->getName()));
          needs_rewriting = true;
        }
        param_info.hoisted_name = std::move(hoisted_name);
        param_infos.push_back(std::move(param_info));
      }

      data.function_map.insert(
          {func_decl, WorkerData::FunctionInfo(std::move(return_info), std::move(param_infos), needs_rewriting)});
    }

    return true;
  }
};
} // namespace CollectFunctionInfo

namespace RewriteFunctions {
using FunctionInfo = CollectFunctionInfo::WorkerData::FunctionInfo;
using FunctionMap = CollectFunctionInfo::WorkerData::FunctionMap;

struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &pa_ctx;
  FunctionMap &function_map;
  NonFFIFunctionDefRewrites &non_ffi_function_def_rewrites;
  NonFFIFunctionDefReturnRewrites &non_ffi_function_def_return_rewrites;
  NonFFIFunctionDeclRewrites &non_ffi_function_decl_rewrites;
  NonFFIFunctionCallRewrites &non_ffi_function_call_rewrites;
  FFIFunctionWrapperRewrites &ffi_rewrites;
  FFIFunctionCallRewrites &ffi_call_rewrites;

  struct FFIFunctionInfo {
    size_t input_buf_size = 0;
    size_t output_buf_size = 0;
    llvm::DenseMap<ParmVarDecl *, std::pair<size_t, size_t>> param_info;
  };
  llvm::DenseMap<FunctionDecl *, FFIFunctionInfo> ffi_function_info;

  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
  FunctionDecl *current_function_decl = nullptr;

  explicit WorkerData(ASTContext &Ctx, PipelineStageCtx &pa_ctx, FunctionMap &function_map,
                      NonFFIFunctionDefRewrites &non_ffi_function_def_rewrites,
                      NonFFIFunctionDefReturnRewrites &non_ffi_function_def_return_rewrites,
                      NonFFIFunctionDeclRewrites &non_ffi_function_decl_rewrites,
                      NonFFIFunctionCallRewrites &non_ffi_function_call_rewrites,
                      FFIFunctionWrapperRewrites &ffi_rewrites, FFIFunctionCallRewrites &ffi_call_rewrites)
      : Ctx(Ctx), pa_ctx(pa_ctx), function_map(function_map),
        non_ffi_function_def_rewrites(non_ffi_function_def_rewrites),
        non_ffi_function_def_return_rewrites(non_ffi_function_def_return_rewrites),
        non_ffi_function_decl_rewrites(non_ffi_function_decl_rewrites),
        non_ffi_function_call_rewrites(non_ffi_function_call_rewrites), ffi_rewrites(ffi_rewrites),
        ffi_call_rewrites(ffi_call_rewrites) {}
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
    os.flush();
    return s;
  }

  static auto GetStorageClassSpecifierString(StorageClass sc) -> auto {
    switch (sc) {
    case SC_None:
      return "";
    case SC_Extern:
      return "extern";
    case SC_Static:
      return "static";
    case SC_PrivateExtern:
      return "__private_extern__";
    case SC_Auto:
      return "auto";
    case SC_Register:
      return "register";
    }
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(func_decl->getBeginLoc())) || !func_decl->isThisDeclarationADefinition()) {
      return true;
    }

    auto it = data.function_map.find(func_decl);
    if (it == data.function_map.end()) {
      data.error = CreateRuntimeError(llvm::formatv("\n    at {0}\nFunctionDecl {1} not found in function_map",
                                                    func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                                    func_decl->getName()));
      return false;
    }

    auto *tmp_function_decl = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto cleanup =
        llvm::scope_exit([this, tmp_function_decl] -> void { data.current_function_decl = tmp_function_decl; });

    for (const auto *attribute : func_decl->attrs()) {
      if (const auto *annotation_attr = llvm::dyn_cast<clang::AnnotateAttr>(attribute)) {
        if (annotation_attr->getAnnotation() == "__c2pnk_rewritten_non_ffi_function") {
          return RecursiveASTVisitor<Worker>::TraverseFunctionDecl(func_decl);
        }
      }
    }

    auto &function_info = it->second;
    auto function_name = func_decl->getName();
    std::string replacement_text;
    llvm::raw_string_ostream os(replacement_text);

    if (func_decl->isThisDeclarationADefinition()) {
      // non-ffi function def

      // add global vars for hoisted return type and parameters
      os << "\n/* c2pancake: start hoisted return type and parameters for function " << func_decl->getName() << " */\n";
      for (auto &param_info : function_info.param_infos) {
        if (param_info.hoisted_name.has_value()) {
          os << "static " << PrintType(param_info.param_decl->getType(), param_info.hoisted_name.value()) << ";\n";
        }
      }
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        if (return_info.hoisted_name.has_value()) {
          os << "static " << PrintType(return_info.return_type, return_info.hoisted_name.value()) << ";\n";
        }
      }
      os << "/* c2pancake: end hoisted return type and parameters for function " << func_decl->getName() << " */\n";

      // now do the function signature

      os << "[[clang::annotate(\"__c2pnk_rewritten_non_ffi_function\")]]\n";

      // function specifiers
      os << GetStorageClassSpecifierString(func_decl->getStorageClass()) << " ";

      // return type
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        if (return_info.hoisted_name.has_value()) {
          // the return var is actually passed through a global var
          // but the function ret types still need to be a uint32/64_t as per pancake rules
          os << GetWordTypeStr(data.Ctx) << " ";
        } else {
          os << return_info.return_type.getAsString() << " ";
        }
      } else {
        // void return type becomes a uint32/64_t as per pancake rules
        os << GetWordTypeStr(data.Ctx) << " ";
      }

      // name
      os << function_name;

      // args
      os << "(";
      for (const auto &param_info : function_info.param_infos) {
        if (!param_info.hoisted_name.has_value()) {
          os << PrintType(param_info.param_decl->getType(), param_info.param_decl->getName()) << ", ";
        }
      }
      os.flush();
      if (replacement_text.ends_with(", ")) {
        replacement_text.resize(replacement_text.size() - 2);
      }
      os << ")";

      // now do the function prologue to load all the hoisted parameters into local variables
      os << " {\n";
      os << "/* c2pancake: start function prologue for function " << function_name << " */\n";
      for (const auto &param_info : function_info.param_infos) {
        if (param_info.hoisted_name.has_value()) {
          os << llvm::formatv("{0} = {1};\n",
                              PrintType(param_info.param_decl->getType(), param_info.param_decl->getName()),
                              param_info.hoisted_name.value());
        }
      }
      os << "/* c2pancake: end function prologue for function " << function_name << " */\n";
      os.flush();

      // replace only "ret_type func_name(param_type1 param1, param_type2 param2, ...) {"
      auto begin_loc = func_decl->getBeginLoc();
      auto l_brace = func_decl->getBody()->getBeginLoc(); // points at '{'
      auto after_l_brace = Lexer::getLocForEndOfToken(l_brace, 0, sm, data.Ctx.getLangOpts());
      auto range = CharSourceRange::getCharRange(begin_loc, after_l_brace);
      auto [_, inserted] = data.non_ffi_function_def_rewrites.insert(
          {func_decl, Replacement(sm, range, replacement_text, data.Ctx.getLangOpts())});
      if (!inserted) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nNon-ffi function definition rewrite for function {1} ALREADY EXISTS",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
        return false;
      }
      os.flush();
    } else {
      // non-ffi function decl

      // function specifiers
      os << GetStorageClassSpecifierString(func_decl->getStorageClass()) << " ";

      // return type
      if (function_info.return_info.has_value()) {
        auto &return_info = function_info.return_info.value();
        if (return_info.hoisted_name.has_value()) {
          // the return var is actually passed through a global var
          // but the function ret types still need to be a uint32/64_t as per pancake rules
          os << GetWordTypeStr(data.Ctx) << " ";
        } else {
          os << return_info.return_type.getAsString() << " ";
        }
      } else {
        // void return type becomes a uint32/64_t as per pancake rules
        os << GetWordTypeStr(data.Ctx) << " ";
      }

      // function name
      os << function_name << "(";

      // parameters
      for (const auto &param_info : function_info.param_infos) {
        if (!param_info.hoisted_name.has_value()) {
          os << PrintType(param_info.param_decl->getType(), param_info.param_decl->getName()) << ", ";
        }
      }
      os.flush();
      if (replacement_text.ends_with(", ")) {
        replacement_text.resize(replacement_text.size() - 2);
      }
      os << ")";

      os.flush();
      if (auto error = data.non_ffi_function_decl_rewrites[func_decl].add(
              {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(func_decl->getSourceRange()),
               replacement_text, data.Ctx.getLangOpts()})) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nFailed to add non-ffi function declaration rewrite for function {1}: {2}",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName(),
                          llvm::fmt_consume(std::move(error))));
        return false;
      };
    }

    return RecursiveASTVisitor<Worker>::TraverseFunctionDecl(func_decl);
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
      data.error =
          CreateRuntimeError(llvm::formatv("\n    at {0}\nCallExpr has no direct callee",
                                           call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager())));
      return false;
    }

    auto callee_name = callee_decl->getName();
    if (callee_name.starts_with("__c2pnk_")) {
      // don't rewrite calls to pancake helper functions
      return true;
    }

    for (const auto *attribute : callee_decl->attrs()) {
      if (const auto *annotation_attr = llvm::dyn_cast<clang::AnnotateAttr>(attribute)) {
        if (annotation_attr->getAnnotation() == "__c2pnk_rewritten_non_ffi_function") {
          return RecursiveASTVisitor<Worker>::TraverseCallExpr(call_expr);
        }
      }
    }

    auto it = data.function_map.find(callee_decl);
    if (it != data.function_map.end()) {
      // non-ffi function call

      auto &function_info = it->second;
      if (function_info.needs_rewriting) {
        // replace with a gnu statement expression that loads the hoisted parameters and return value
        std::string hoisted_parameters;
        llvm::raw_string_ostream os(hoisted_parameters);
        os << "({\n";

        std::string func_call;
        llvm::raw_string_ostream os2(func_call);

        if (function_info.return_info.has_value()) {
          auto &return_info = function_info.return_info.value();
          if (return_info.hoisted_name.has_value()) {
            // the return value is being passed through a global var
            // this means the function now returns a uint32/64_t as per pancake rules
            // we cast the result to void since we never use it
            os2 << "(void)";
          }
        }

        os2 << callee_name << "(";

        for (const auto &[i, param_info] : std::views::enumerate(function_info.param_infos)) {
          auto *arg_i = call_expr->getArg((unsigned)i);
          auto arg_i_src = GetSourceText(arg_i, data.Ctx);
          if (auto error = arg_i_src.takeError()) {
            data.error = CreateRuntimeError(
                llvm::formatv("\n    at {0}\nFailed to get source text for argument {1} of CallExpr: {2}",
                              arg_i->getBeginLoc().printToString(data.Ctx.getSourceManager()), i,
                              llvm::fmt_consume(std::move(error))));
            return false;
          }
          if (param_info.hoisted_name.has_value()) {
            os << llvm::formatv("{0} = {1};\n", param_info.hoisted_name.value(), *arg_i_src);
          } else {
            os2 << *arg_i_src << ", ";
          }
        }

        os2.flush();
        if (func_call.ends_with(", ")) {
          func_call.resize(func_call.size() - 2);
        }
        os2 << ")";
        os2.flush();

        os << func_call << ";\n";
        if (function_info.return_info.has_value()) {
          auto &return_info = function_info.return_info.value();
          if (return_info.hoisted_name.has_value()) {
            os << llvm::formatv("{0};\n", return_info.hoisted_name.value());
          }
        }
        os << "})";
        os.flush();

        if (auto error = data.non_ffi_function_call_rewrites[callee_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(call_expr->getSourceRange()),
                 hoisted_parameters, data.Ctx.getLangOpts()})) {
          data.error = CreateRuntimeError(
              llvm::formatv("\n    at {0}\nFailed to add rewrite for non-FFI function call: {1}\n    Did you "
                            "recursively call this function? If so, c2pancake does not support that yet.",
                            call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                            llvm::fmt_consume(std::move(error))));
          return false;
        }
      }
    } else {
      // ffi function call
      /*
      ret_type example_func(arg1, arg2, arg3, arg4, arg5);

      gets turned into

      #include <stdint.h>
      uint64/32_t __cp2nk_memcpy(uint8_t *dest, uint8_t *src, uint64/32_t len) {
        while (len > 0UL) {
          *dest = *src;
          dest = dest + 1UL;
          src = src + 1UL;
          len = len - 1UL;
        }
        return 0;
      }

      static uint8_t __c2pnk_ffi_example_func_input_buf[CALCULATED_SIZE];
      static uint8_t __c2pnk_ffi_example_func_output_buf[CALCULATED_SIZE];

      // actual call expression gets rewritten to the following. note all the args have to be located on the heap!
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf, &arg1, sizeof(arg1));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, &arg2, sizeof(arg2));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, &arg3, sizeof(arg3));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, &arg4, sizeof(arg4));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, &arg5, sizeof(arg5));
      // actual function call
      __c2pnk_ffi_example_func(__c2pnk_ffi_example_func_input_buf, sizeof(__c2pnk_ffi_example_func_input_buf),
      __c2pnk_ffi_example_func_output_buf, sizeof(__c2pnk_ffi_example_func_output_buf));
      __c2pnk_memcpy(&ret_val, __c2pnk_ffi_example_func_output_buf, sizeof(ret_val));

      // the function below will be located in the ffi_stubs file!!!
      int __c2pnk_ffi_example_func(uint8_t *input_buf, uint64/32_t input_buf_len, uint8_t *output_buf, uint64/32_t
      output_buf_len) {
        arg1;
        memcpy(&input_buf[0], &arg1, sizeof(arg1));
        arg2;
        memcpy(&input_buf[CALCULATED_OFFSET], &arg2, sizeof(arg2));
        arg3;
        memcpy(&input_buf[CALCULATED_OFFSET], &arg3, sizeof(arg3));
        arg4;
        memcpy(&input_buf[CALCULATED_OFFSET], &arg4, sizeof(arg4));
        arg5;
        memcpy(&input_buf[CALCULATED_OFFSET], &arg5, sizeof(arg5));
        ret_val = example_func(arg1, arg2, arg3, arg4, arg5);
        memcpy(&ret_val, &output_buf[0], sizeof(ret_val));
        return 0;
      }
      */
      auto it = data.ffi_rewrites.find(callee_decl);
      if (callee_decl->isVariadic()) {
        // create a new name mangled variadic version of the function every time...
      } else if (it == data.ffi_rewrites.end()) {
        // create ffi "stub" for this function

        // get size of input args
        size_t input_buf_size = 0;
        llvm::DenseMap<ParmVarDecl *, std::pair<size_t, size_t>> param_offsets_sizes; // param_decl -> (offset, size)
        for (const auto &param_info : callee_decl->parameters()) {
          auto param_type = param_info->getType();
          auto type_size = data.Ctx.getTypeSize(param_type);
          if (type_size % 8 != 0) {
            data.error = CreateRuntimeError(llvm::formatv(
                "\n    at {0}\nFFI function {1} has parameter {2} of type {3} with size {4} bits, which is not a "
                "multiple of 8 bits",
                param_info->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName(),
                param_info->getName(), param_type.getAsString(), type_size));
            return false;
          }
          param_offsets_sizes.insert({param_info, {input_buf_size, type_size / 8}});
          input_buf_size += (data.Ctx.getTypeSize(param_type) / 8);
        }

        size_t output_buf_size = 0;
        auto return_type = callee_decl->getReturnType();
        if (!return_type->isVoidType()) {
          auto type_size = data.Ctx.getTypeSize(return_type);
          if (type_size % 8 != 0) {
            data.error = CreateRuntimeError(llvm::formatv(
                "\n    at {0}\nFFI function {1} has return type {2} with size {3} bits, which is not a multiple of 8 "
                "bits",
                callee_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName(),
                return_type.getAsString(), type_size));
            return false;
          }
          output_buf_size = (type_size / 8);
        }

        data.ffi_function_info.insert({callee_decl, WorkerData::FFIFunctionInfo{.input_buf_size = input_buf_size,
                                                                                .output_buf_size = output_buf_size,
                                                                                .param_info = param_offsets_sizes}});

        auto func_name = callee_decl->getName();
        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);

        // add #include where callee_decl is located
        auto &sm = data.Ctx.getSourceManager();
        auto loc = sm.getFileLoc(callee_decl->getLocation());
        auto file_id = sm.getFileID(loc);
        if (file_id == sm.getMainFileID()) {
          data.error = CreateRuntimeError(llvm::formatv(
              "\n    at {0}\nFFI function {1} is defined in the current source file which doesn't make sense",
              callee_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()));
          return false;
        }
        auto file_entry = sm.getFileEntryRefForID(file_id);
        if (!file_entry.has_value()) {
          data.error = CreateRuntimeError(llvm::formatv(
              "\n    at {0}\nCouldn't get header file where FFI function {1} resides",
              callee_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()));
          return false;
        }
        os << llvm::formatv("#include <{0}>\n", file_entry->getName());

        // add global buffers
        os << llvm::formatv("#include <stdint.h>\n#include <string.h>\n");
        os << llvm::formatv("static uint8_t __c2pnk_ffi_input_buf_{0}[{1}];\n", func_name, input_buf_size);
        if (return_type->isVoidType()) {
          os << llvm::formatv("static uint8_t __c2pnk_ffi_output_buf_{0}[1]; /* placeholder for void return */\n",
                              func_name);
        } else {
          os << llvm::formatv("static uint8_t __c2pnk_ffi_output_buf_{0}[{1}];\n", func_name, output_buf_size);
        }

        os << llvm::formatv("static {0} __c2pnk_ffi_{1}(uint8_t *input_buf, {0} input_buf_len, uint8_t *output_buf, "
                            "{0} output_buf_len) {{\n",
                            GetWordTypeStr(data.Ctx), func_name);

        // create and init local vars to pass to actual function
        for (const auto &[i, param_info] : std::views::enumerate(callee_decl->parameters())) {
          auto param_type = param_info->getType();
          auto [offset, size] = param_offsets_sizes.at(param_info);

          os << PrintType(param_type, param_info->getName()) << ";\n";
          os << llvm::formatv("memcpy(&{0}, &input_buf[{1}], {2});\n", param_info->getName(), offset, size);
        }

        if (!return_type->isVoidType()) {
          os << PrintType(return_type, "__c2pnk_ret_val") << ";\n";
        }
        // call the actual function
        if (!return_type->isVoidType()) {
          os << llvm::formatv("__c2pnk_ret_val = {0}(", func_name);
        } else {
          os << llvm::formatv("{0}(", func_name);
        }
        std::string arg_list;
        llvm::raw_string_ostream os2(arg_list);
        for (const auto &param_info : callee_decl->parameters()) {
          os2 << param_info->getName() << ", ";
        }
        os2.flush();
        if (arg_list.ends_with(", ")) {
          arg_list.resize(arg_list.size() - 2);
        }
        os << arg_list << ");\n";

        // copy return value to output buffer
        if (!return_type->isVoidType()) {
          os << llvm::formatv("memcpy(&output_buf[0], &__c2pnk_ret_val, {0});\n", output_buf_size);
        }
        os << "return 0;\n";
        os << "}\n";
        os.flush();

        data.ffi_rewrites.insert(
            {callee_decl, Replacement(sm, sm.getLocForStartOfFile(sm.getMainFileID()), 0, replacement_text)});
      }

      // now rewrite the call expr using a gnu statement expression
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      os << "({\n";

      // copy all the args to the input buffer
      for (const auto &[i, param_info] : std::views::enumerate(callee_decl->parameters())) {
        auto param_type = param_info->getType();
        auto *arg_i = call_expr->getArg((unsigned)i)->IgnoreParenImpCasts();
        auto arg_i_src = GetSourceText(arg_i, data.Ctx);
        if (auto error = arg_i_src.takeError()) {
          data.error = CreateRuntimeError(llvm::formatv(
              "\n    at {0}\nFailed to get source text for argument {1} of CallExpr: {2}",
              arg_i->getBeginLoc().printToString(data.Ctx.getSourceManager()), i, llvm::fmt_consume(std::move(error))));
          return false;
        }
        auto [offset, size] = data.ffi_function_info[callee_decl].param_info[param_info];
        if (!callee_decl->isVariadic()) {
          if (!arg_i->isLValue()) {
            // requires tmp var to hold the value of the argument so we can take the address of it
            std::string tmp_var_name = GetTempVarName(llvm::formatv("arg_{0}", i));
            os << llvm::formatv("{0} = {1};\n", PrintType(param_type, tmp_var_name), *arg_i_src);
            os << llvm::formatv("__c2pnk_memcpy(__c2pnk_ffi_input_buf_{0} + {1}, &{2}, {3});\n", callee_name, offset,
                                tmp_var_name, size);
          } else {
            os << llvm::formatv("__c2pnk_memcpy(__c2pnk_ffi_input_buf_{0} + {1}, &{2}, {3});\n", callee_name, offset,
                                *arg_i_src, size);
          }
        } else {
          // TODO variadic functions...
        }
      }

      // call the ffi stub function
      if (callee_decl->isVariadic()) {
        // TODO variadic functions...
      } else {
        os << llvm::formatv("__c2pnk_ffi_{0}(__c2pnk_ffi_input_buf_{0}, sizeof(__c2pnk_ffi_input_buf_{0}), "
                            "__c2pnk_ffi_output_buf_{0}, sizeof(__c2pnk_ffi_output_buf_{0}));\n",
                            callee_name);
      }

      if (!callee_decl->getReturnType()->isVoidType()) {
        os << PrintType(callee_decl->getReturnType(), "__c2pnk_ret_val") << ";\n";
        os << llvm::formatv(
            "memcpy(&__c2pnk_ret_val, __c2pnk_ffi_output_buf_{0}, sizeof(__c2pnk_ffi_output_buf_{0}));\n", callee_name);
        os << "__c2pnk_ret_val;\n";
      }

      os << "})";
      os.flush();

      if (auto error = data.ffi_call_rewrites[callee_decl].add(
              {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(call_expr->getSourceRange()),
               replacement_text, data.Ctx.getLangOpts()})) {
        data.error = CreateRuntimeError(llvm::formatv(
            "\n    at {0}\nFailed to add rewrite for FFI function call: {1}\n    Did you "
            "recursively call this function? If so, c2pancake does not support that yet.",
            call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), llvm::fmt_consume(std::move(error))));
        return false;
      }
    }

    return RecursiveASTVisitor<Worker>::TraverseCallExpr(call_expr);
  };

  auto TraverseReturnStmt(ReturnStmt *return_stmt) -> bool {
    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(return_stmt->getBeginLoc()))) {
      return true;
    }

    if (data.current_function_decl == nullptr) {
      data.error = CreateRuntimeError(llvm::formatv("\n    at {0}\nReturnStmt found outside of a FunctionDecl",
                                                    return_stmt->getBeginLoc().printToString(sm)));
      return false;
    }

    auto it = data.function_map.find(data.current_function_decl);
    if (it == data.function_map.end()) {
      data.error = CreateRuntimeError(
          llvm::formatv("\n    at {0}\nFunctionDecl {1} not found in function_map",
                        data.current_function_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                        data.current_function_decl->getName()));
      return false;
    }

    auto &function_info = it->second;
    if (function_info.return_info.has_value()) {
      auto &return_info = function_info.return_info.value();
      if (return_info.hoisted_name.has_value()) {
        // rewrite the return statement to assign to the hoisted return variable
        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);
        os << "{\n";
        os << llvm::formatv("{0} = ", *return_info.hoisted_name);
        if (return_stmt->getRetValue() != nullptr) {
          auto ret_value_src = GetSourceText(return_stmt->getRetValue(), data.Ctx);
          if (auto error = ret_value_src.takeError()) {
            data.error =
                CreateRuntimeError(llvm::formatv("\n    at {0}\nFailed to get source text for ReturnStmt: {1}",
                                                 return_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                                 llvm::fmt_consume(std::move(error))));
            return false;
          }
          os << *ret_value_src;
        }
        os << ";\n";
        // return 0UL instead as per pancake rules
        os << "return 0UL;\n";
        os << "}";
        os.flush();

        if (auto error = data.non_ffi_function_def_return_rewrites[data.current_function_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(return_stmt->getSourceRange()),
                 replacement_text, data.Ctx.getLangOpts()})) {
          data.error = std::move(error);
          return false;
        };
      }
    }

    return RecursiveASTVisitor<Worker>::TraverseReturnStmt(return_stmt);
  }
};
} // namespace RewriteFunctions
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  CollectFunctionInfo::WorkerData::FunctionMap function_map;
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

  NonFFIFunctionDefRewrites non_ffi_function_def_rewrites;
  NonFFIFunctionDefReturnRewrites non_ffi_function_def_return_rewrites;
  NonFFIFunctionDeclRewrites non_ffi_function_decl_rewrites;
  NonFFIFunctionCallRewrites non_ffi_function_call_rewrites;
  FFIFunctionWrapperRewrites ffi_rewrites;
  FFIFunctionCallRewrites ffi_call_rewrites;
  {
    RewriteFunctions::WorkerData data(Ctx, ps_ctx, function_map, non_ffi_function_def_rewrites,
                                      non_ffi_function_def_return_rewrites, non_ffi_function_decl_rewrites,
                                      non_ffi_function_call_rewrites, ffi_rewrites, ffi_call_rewrites);
    RewriteFunctions::Worker w(data);
    w.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (data.error) {
      ps_ctx.error = std::move(data.error);
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  /*
  note that for a given FunctionDecl * in non_ffi_function_def_rewrites, all replacements in
  non_ffi_function_def_return_rewrites, non_ffi_function_decl_rewrites and non_ffi_function_call_rewrites with the same
  FunctionDecl * PLUS the replacement in non_ffi_function_def_rewrites need to be atomically applied together.
  Otherwise, continue and try and apply the next FunctionDecl * and then ultimately repeat the pass.

  The reasoning is that if we can't atomically apply all related replacements for a given FunctionDecl *,
  then the file is left in an inconsistent state and we won't able to properly parse it in the next pass.
  This "error" might happen if the user has nested function calls and the inner function call is rewritten first
  and then the outer function call is rewritten next but the 2 replacements are not order-independent.

  The only truly irrecoverable case is if the user recursively calls the same function.

  side comment: cock and balls because llvm hides the function mergeIfOrderIndependent as private but that's
  the exact function I want to use here!!!
  */
  Replacements all_rewrites;
  bool has_conflict = false;
  for (const auto &[func_decl, def_replacement] : non_ffi_function_def_rewrites) {
    Replacements tmp_replacements = all_rewrites; // this is an O(n^2) algorithm :skull:
    bool has_internal_conflict = false;
    if (auto error = tmp_replacements.add(def_replacement)) {
      ps_ctx.error = CreateRuntimeError(llvm::formatv("Unexpected add replacement conflict: {0}", error));
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }

    if (!has_internal_conflict) {
      auto ret_it = non_ffi_function_def_return_rewrites.find(func_decl);
      if (ret_it != non_ffi_function_def_return_rewrites.end()) {
        for (const auto &ret_replacement : ret_it->second) {
          if (tmp_replacements.add(ret_replacement)) {
            has_conflict = true;
            has_internal_conflict = true;
            break;
          }
        }
      }
    }

    if (!has_internal_conflict) {
      auto decl_it = non_ffi_function_decl_rewrites.find(func_decl);
      if (decl_it != non_ffi_function_decl_rewrites.end()) {
        for (const auto &decl_replacement : decl_it->second) {
          if (tmp_replacements.add(decl_replacement)) {
            has_conflict = true;
            has_internal_conflict = true;
            break;
          }
        }
      }
    }

    if (!has_internal_conflict) {
      auto call_it = non_ffi_function_call_rewrites.find(func_decl);
      if (call_it != non_ffi_function_call_rewrites.end()) {
        for (const auto &call_replacement : call_it->second) {
          if (tmp_replacements.add(call_replacement)) {
            has_conflict = true;
            has_internal_conflict = true;
            break;
          }
        }
      }
    }

    if (!has_internal_conflict) {
      all_rewrites = std::move(tmp_replacements);
    }
  }

  // now do ffi functions
  for (const auto &[func_decl, ffi_replacement] : ffi_rewrites) {
    Replacements tmp_replacements = all_rewrites; // this is an O(n^2) algorithm :skull:
    bool has_internal_conflict = false;
    if (auto error = tmp_replacements.add(ffi_replacement)) {
      // unfortunately, all the ffi rewrites are added to the top of the file with [0,0) range
      // this means they'll all collide with each other...
      // so if there are ffi rewrites, only one can be done per pass
      has_conflict = true;
      has_internal_conflict = true;
      break;
    }

    if (!has_internal_conflict) {
      auto call_it = ffi_call_rewrites.find(func_decl);
      if (call_it != ffi_call_rewrites.end()) {
        for (const auto &call_replacement : call_it->second) {
          if (tmp_replacements.add(call_replacement)) {
            has_conflict = true;
            has_internal_conflict = true;
            break;
          }
        }
      }
    }

    if (!has_internal_conflict) {
      all_rewrites = std::move(tmp_replacements);
    }
  }

  for (const auto &r : all_rewrites) {
    llvm::outs() << "===\n";
    llvm::outs() << "File path: " << r.getFilePath() << "\n";
    llvm::outs() << "Offset: " << r.getOffset() << "\n";
    llvm::outs() << "Length: " << r.getLength() << "\n";
    llvm::outs() << "Text: " << r.getReplacementText() << "\n";
  }

  ps_ctx.replacements = std::move(all_rewrites);
  if (has_conflict) {
    ps_ctx.whats_next = WhatsNext::RepeatPass;
  } else {
    ps_ctx.whats_next = WhatsNext::MoveToNextPass;
  }
}
} // namespace pancake::pass_function_calling
