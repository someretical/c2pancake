#include "Pass_FunctionCalling.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attrs.inc>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::tooling;

namespace pancake::pass_inject_memcpy_polyfill {
namespace {
const char *memcpy_polyfill_code = R"(
#include <stdint.h>
static inline uint{0}_t __c2pnk_memcpy(uint8_t *dest, uint8_t *src, uint{0}_t len) {{
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
  if (auto error = ps_ctx.replacements.add({sm, sm.getLocForStartOfFile(sm.getMainFileID()), 0,
                                            llvm::formatv(memcpy_polyfill_code, GetPointerWidth(Ctx)).str()})) {
    ps_ctx.error = llvm::joinErrors(CreateRuntimeError("Add replacement conflict"), std::move(error));
    ps_ctx.whats_next = WhatsNext::MoveToNextFile;
    return;
  }
  ps_ctx.whats_next = WhatsNext::MoveToNextPass;
}
} // namespace pancake::pass_inject_memcpy_polyfill

namespace pancake::pass_function_calling {
namespace {
// all rewrites for a non-ffi function
// contains definition rewrites, return rewrites, declaration rewrites, and call expr rewrites
// also captures the more invasive rewrites done for external entry points
// needs to preserve insertion order because we apply the innermost rewrites first
using NonFFIFunctionRewrites = llvm::MapVector<FunctionDecl *, Replacements>;

// ffi function rewrites
// contains the ffi wrappers, and call expression rewrites
// needs to preserve insertion order because we apply the innermost rewrites first
using FFIFunctionRewrites = llvm::MapVector<FunctionDecl *, Replacements>;

// ffi variadic function rewrites
// these are keyed be the call expr * to provide something unique every pass
using FFIVariadicFunctionRewrites = llvm::MapVector<CallExpr *, Replacements>;

namespace CollectFunctionInfo {
struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;

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
    bool turn_return_from_void_to_uint = false;
    explicit FunctionInfo(std::optional<ReturnInfo> return_info, llvm::SmallVector<ParamInfo, 16> param_infos,
                          bool needs_rewriting, bool turn_return_from_void_to_uint)
        : return_info(std::move(return_info)), param_infos(std::move(param_infos)), needs_rewriting(needs_rewriting),
          turn_return_from_void_to_uint(turn_return_from_void_to_uint) {}
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
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.ps_ctx.major_pass_number, data.ps_ctx.minor_pass_number,
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
      std::optional<WorkerData::ReturnInfo> return_info = std::nullopt;
      if (!return_type->isIntegerType() && !return_type->isPointerType() && !return_type->isVoidType()) {
        // hoist the return type
        auto hoisted_name = llvm::SmallString<32>(llvm::formatv("__c2pnk_{0}_return", func_decl->getName()));
        return_info = WorkerData::ReturnInfo(return_type);
        return_info.value().hoisted_name = std::move(hoisted_name);
        needs_rewriting = true;
      }

      bool turn_return_from_void_to_uint = false;
      if (return_type->isVoidType()) {
        // function needs to return 0UL as per pancake rules
        turn_return_from_void_to_uint = true;
      }

      llvm::SmallVector<WorkerData::ParamInfo, 16> param_infos;
      for (const auto [i, param_decl] : func_decl->parameters() | std::views::enumerate) {
        auto param_type = param_decl->getType();
        if ((param_type->isIntegerType() || param_type->isPointerType()) &&
            data.Ctx.getTypeSize(param_type) > GetPointerWidth(data.Ctx)) {
          data.error = CreateRuntimeError(std::move(llvm::formatv(
              "\n    at {0}\nFunctionDecl {1} has integer parameter {2} of type {3} larger than pointer width",
              param_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName(),
              param_decl->getName(), param_type.getAsString())));
          return false;
        }

        auto param_info = WorkerData::ParamInfo(param_decl);
        std::optional<llvm::SmallString<32>> hoisted_name = std::nullopt;
        if (!param_type->isIntegerType() && !param_type->isPointerType() && !param_type->isVoidType()) {
          // hoist the parameter type
          hoisted_name = llvm::SmallString<32>(
              llvm::formatv("__c2pnk_{0}_param{1}_{2}", func_decl->getName(), i, param_decl->getName()));
          needs_rewriting = true;
        }
        param_info.hoisted_name = std::move(hoisted_name);
        param_infos.push_back(std::move(param_info));
      }

      data.function_map.insert({func_decl, WorkerData::FunctionInfo(std::move(return_info), std::move(param_infos),
                                                                    needs_rewriting, turn_return_from_void_to_uint)});
    }

    return true;
  }
};
} // namespace CollectFunctionInfo

/*
Annotation types:
[[clang::annotate("__c2pnk_rewritten_non_ffi_function")]]: marks a function signature as being rewritten due to passing
structs by value.
[[clang::annotate("__c2pnk_external_entry_point")]]: marks a function as being an external entry point for the pancake
runtime. Function definitions with this signature will not be touched. However, their body will be replaced with a call
to a new function which does the same thing but adheres to pancake calling convention. During the final translation
stage, the function marked with this annotation will be removed entirely.
 */
namespace RewriteFunctions {
using FunctionInfo = CollectFunctionInfo::WorkerData::FunctionInfo;
using FunctionMap = CollectFunctionInfo::WorkerData::FunctionMap;

struct FFIFunctionWrapperParamInfo {
  ParmVarDecl *original_param_decl;
  size_t original_param_index;
  std::optional<std::pair<size_t, size_t>> offset_and_size;
  // if not present, then it's on the fast path
  // otherwise, the first element is the offset in bytes from the start of the input buffer, and the second element is
  // the size in bytes
  explicit FFIFunctionWrapperParamInfo(ParmVarDecl *original_param_decl, size_t original_param_index,
                                       std::optional<std::pair<size_t, size_t>> offset_and_size = std::nullopt)
      : original_param_decl(original_param_decl), original_param_index(original_param_index),
        offset_and_size(std::move(offset_and_size)) {}
};

struct FFIFunctionWrapperReturnInfo {
  QualType original_return_type;
  std::optional<size_t> size_in_bytes;

  explicit FFIFunctionWrapperReturnInfo(QualType original_return_type,
                                        std::optional<size_t> size_in_bytes = std::nullopt)
      : original_return_type(original_return_type), size_in_bytes(size_in_bytes) {}

  // "fast path" (meaning no memcpy but global var still needed)
  auto IsFastPath() const -> bool { return !size_in_bytes.has_value(); }
};

struct FFIFunctionWrapperInfo {
  std::array<std::optional<FFIFunctionWrapperParamInfo>, 4> param_infos;
  llvm::SmallVector<FFIFunctionWrapperParamInfo> non_fast_path_param_infos;
  llvm::SmallVector<std::pair<std::optional<size_t>, std::optional<size_t>>> param_map;
  size_t non_fastpath_input_buf_size; // compile-time constant, hardcoded into generated code, no slot
  FFIFunctionWrapperReturnInfo return_info;

  explicit FFIFunctionWrapperInfo(std::array<std::optional<FFIFunctionWrapperParamInfo>, 4> param_infos,
                                  llvm::SmallVector<FFIFunctionWrapperParamInfo> non_fast_path_param_infos,
                                  llvm::SmallVector<std::pair<std::optional<size_t>, std::optional<size_t>>> param_map,
                                  size_t non_fastpath_input_buf_size, FFIFunctionWrapperReturnInfo return_info)
      : param_infos(std::move(param_infos)), non_fast_path_param_infos(std::move(non_fast_path_param_infos)),
        param_map(std::move(param_map)), non_fastpath_input_buf_size(non_fastpath_input_buf_size),
        return_info(return_info) {}

  // Only two possible reserved slots now: output_buf, then input_buf. Lengths are compile-time
  // constants (return_info.size_in_bytes / non_fastpath_input_buf_size) baked directly into the
  // generated wrapper body, so they never occupy a slot.
  auto HasOutputBufSlot() const -> bool { return !return_info.original_return_type->isVoidType(); }
  auto HasInputSlot() const -> bool { return !non_fast_path_param_infos.empty(); }

  auto OutputBufSlotIndex() const -> std::optional<size_t> {
    if (!HasOutputBufSlot())
      return std::nullopt;
    return 0;
  }
  auto InputBufSlotIndex() const -> std::optional<size_t> {
    if (!HasInputSlot())
      return std::nullopt;
    return HasOutputBufSlot() ? 1 : 0;
  }
};

struct WorkerData {
  ASTContext &Ctx;
  PipelineStageCtx &ps_ctx;
  FunctionMap &function_map;
  NonFFIFunctionRewrites &non_ffi_rewrites;
  FFIFunctionRewrites &ffi_rewrites;
  FFIVariadicFunctionRewrites &ffi_variadic_rewrites;

  llvm::DenseMap<const FunctionDecl *, FFIFunctionWrapperInfo> ffi_function_info;

  llvm::Error error = llvm::Error::success();
  size_t tmp_var_counter = 0;
  FunctionDecl *current_function_decl = nullptr;

  explicit WorkerData(ASTContext &Ctx, PipelineStageCtx &ps_ctx, FunctionMap &function_map,
                      NonFFIFunctionRewrites &non_ffi_rewrites, FFIFunctionRewrites &ffi_rewrites,
                      FFIVariadicFunctionRewrites &ffi_variadic_rewrites)
      : Ctx(Ctx), ps_ctx(ps_ctx), function_map(function_map), non_ffi_rewrites(non_ffi_rewrites),
        ffi_rewrites(ffi_rewrites), ffi_variadic_rewrites(ffi_variadic_rewrites) {}
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return false; }

  auto GetTempVarName(std::string hint) -> auto {
    return llvm::formatv("__c2pnk_{0}_{1}_{2}_{3}", hint, data.ps_ctx.major_pass_number, data.ps_ctx.minor_pass_number,
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

  auto ConstructFunctionWrapperInfo(const FunctionDecl *func_decl) const -> FFIFunctionWrapperInfo {
    std::array<std::optional<FFIFunctionWrapperParamInfo>, 4> param_infos{};
    llvm::SmallVector<FFIFunctionWrapperParamInfo> non_fast_path_param_infos;
    llvm::SmallVector<std::pair<std::optional<size_t>, std::optional<size_t>>> param_map;
    std::optional<FFIFunctionWrapperReturnInfo> return_info;

    auto pointer_width = GetPointerWidth(data.Ctx);

    auto fits_fastpath = [&](QualType type) -> bool {
      auto size = data.Ctx.getTypeSize(type) / 8;
      return (type->isIntegerType() || type->isPointerType()) && size <= pointer_width;
    };

    // process return type first
    auto return_type = func_decl->getReturnType();
    if (return_type->isVoidType()) {
      return_info = FFIFunctionWrapperReturnInfo(return_type);
    } else {
      auto return_type_size = data.Ctx.getTypeSize(return_type) / 8;
      if (fits_fastpath(return_type)) {
        // fits directly: no memcpy needed, just a typed store through the output pointer
        return_info = FFIFunctionWrapperReturnInfo(return_type);
      } else {
        // too big: needs a memcpy into the caller-provided output buffer
        return_info = FFIFunctionWrapperReturnInfo(return_type, return_type_size);
      }
    }

    // reserved slots always sit at a fixed prefix: output_buf [, input_buf].
    // lengths are compile-time constants, so they never take a slot.
    size_t fastpath_index_counter = 0;
    if (!return_type->isVoidType()) {
      fastpath_index_counter++; // output_buf
    }

    auto num_params = func_decl->getNumParams();
    size_t remaining_slots_after_output = 4 - fastpath_index_counter;
    bool needs_input_buffer = num_params > remaining_slots_after_output;
    if (!needs_input_buffer) {
      for (const auto &param_decl : func_decl->parameters()) {
        if (!fits_fastpath(param_decl->getType())) {
          needs_input_buffer = true;
          break;
        }
      }
    }
    if (needs_input_buffer) {
      fastpath_index_counter++; // input_buf
    }

    // place each parameter: fastpath-eligible params fill whatever slots remain, in order
    // anything left over (type-ineligible, or the fastpath slots ran out) goes into the
    // non-fastpath input buffer, with its offset/size recorded and padded to the param's
    // required alignment so structs (and anything else with alignment > 1) land correctly.
    size_t current_input_buf_offset = 0;
    for (const auto [i, param_decl] : func_decl->parameters() | std::views::enumerate) {
      auto param_type = param_decl->getType();
      auto param_type_size = data.Ctx.getTypeSize(param_type) / 8;
      if (fits_fastpath(param_type) && fastpath_index_counter < 4) {
        param_infos.at(fastpath_index_counter) = FFIFunctionWrapperParamInfo(param_decl, static_cast<size_t>(i));
        param_map.push_back(std::make_pair(fastpath_index_counter, std::nullopt));
        fastpath_index_counter++;
      } else {
        auto param_align = data.Ctx.getTypeAlign(param_type) / 8; // bits -> bytes
        current_input_buf_offset = llvm::alignTo(current_input_buf_offset, param_align);

        non_fast_path_param_infos.push_back(FFIFunctionWrapperParamInfo(
            param_decl, static_cast<size_t>(i), std::make_pair(current_input_buf_offset, param_type_size)));
        param_map.push_back(std::make_pair(std::nullopt, non_fast_path_param_infos.size() - 1));
        current_input_buf_offset += param_type_size;
      }
    }

    return FFIFunctionWrapperInfo(std::move(param_infos), std::move(non_fast_path_param_infos), std::move(param_map),
                                  current_input_buf_offset, *return_info);
  }

  auto GenerateFFIWrapperFunction(const FunctionDecl *func_decl, llvm::raw_ostream &os) const -> llvm::Error {
    auto info = ConstructFunctionWrapperInfo(func_decl);
    const auto &policy = data.Ctx.getPrintingPolicy();

    auto func_name = func_decl->getName();
    auto wrapper_name = "__c2pnk_ffi_wrapper_" + func_name;

    // add #include where func_decl is located
    auto &sm = data.Ctx.getSourceManager();
    auto loc = sm.getFileLoc(func_decl->getLocation());
    auto file_id = sm.getFileID(loc);
    if (file_id != sm.getMainFileID()) {
      // sometimes external func decls can appear in the same file and only be specified during link time...
      // return CreateRuntimeError(std::move(
      //     llvm::formatv("\n    at {0}\nFFI function {1} is defined in the current source file which doesn't make
      //     sense",
      //                   func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_name)));
      auto file_entry = sm.getFileEntryRefForID(file_id);
      if (!file_entry.has_value()) {
        return CreateRuntimeError(
            std::move(llvm::formatv("\n    at {0}\nCouldn't get header file where FFI function {1} resides",
                                    func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_name)));
      }
      auto &file_manager = sm.getFileManager();
      os << llvm::formatv("#include <{0}>\n", file_manager.getCanonicalName(*file_entry));
      os << llvm::formatv("#include <stdint.h>\n#include <string.h>\n");
    }

    // create the global buffers for the non-fastpath params and return value, if needed
    if (auto idx = info.InputBufSlotIndex()) {
      os << llvm::formatv("static uint8_t __c2pnk_ffi_input_buf_{0}[{1}];\n", func_name,
                          info.non_fastpath_input_buf_size);
    }
    if (auto idx = info.OutputBufSlotIndex()) {
      if (info.return_info.IsFastPath()) {
        std::string return_var_name = "__c2pnk_ffi_output_buf_" + func_name.str();
        os << llvm::formatv("static {0};\n", PrintType(info.return_info.original_return_type, return_var_name));
      } else {
        os << llvm::formatv("static uint8_t __c2pnk_ffi_output_buf_{0}[{1}];\n", func_name,
                            info.return_info.size_in_bytes.value());
      }
    }

    // emit the wrapper function signature
    os << "void " << wrapper_name << "(uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3) {\n";

    // pull out the reserved slots, if present
    if (auto idx = info.InputBufSlotIndex()) {
      os << "/* input_buf = arg" << *idx << " */\n";
    }

    // build the argument list for the call, in original parameter order
    std::string call_args;
    llvm::raw_string_ostream call_args_os(call_args);
    for (size_t orig_idx = 0; orig_idx < info.param_map.size(); orig_idx++) {
      if (orig_idx != 0) {
        call_args_os << ", ";
      }
      auto [fastpath_slot, non_fastpath_idx] = info.param_map[orig_idx];

      if (fastpath_slot.has_value()) {
        auto &param_info = *info.param_infos.at(*fastpath_slot);
        auto param_type = param_info.original_param_decl->getType();
        auto type_str = param_type.getAsString(policy);
        call_args_os << "(" << type_str << ")arg" << *fastpath_slot;
      } else {
        auto idx = info.InputBufSlotIndex().value();
        auto &param_info = info.non_fast_path_param_infos[*non_fastpath_idx];
        auto param_type = param_info.original_param_decl->getType();
        auto type_str = data.Ctx.getPointerType(param_type).getAsString(policy);
        auto [offset, size] = *param_info.offset_and_size;
        call_args_os << "(*(" << type_str << " )(arg" << idx << " + " << offset << "UL))";
      }
    }
    call_args_os.flush();

    // emit the call and write the return value out, choosing the cheapest path available:
    //   - void: just call it
    //   - fastpath: store the call result straight through the output pointer, no temp needed
    //   - non-fastpath: memcpy needs an addressable source, so a local temp is unavoidable here
    if (info.return_info.original_return_type->isVoidType()) {
      os << func_decl->getNameAsString() << "(" << call_args << ");\n";
    } else {
      auto ret_ptr_type = data.Ctx.getPointerType(info.return_info.original_return_type);
      auto idx = info.OutputBufSlotIndex().value();
      os << " /* output_buf = arg" << idx << " */\n";
      os << "*(" << ret_ptr_type.getAsString(data.Ctx.getPrintingPolicy()) << ")arg" << idx << " = "
         << func_decl->getNameAsString() << "(" << call_args << ");\n";
    }

    os << "}\n";
    os.flush();
    return llvm::Error::success();
  }

  auto CallExprToFFIWrapperStmtExpr(const CallExpr *call_expr, const FunctionDecl *func_decl, llvm::raw_ostream &os)
      -> llvm::Error {
    auto func_name = func_decl->getName();
    auto &info = data.ffi_function_info.find(func_decl)->second;
    const auto &policy = data.Ctx.getPrintingPolicy();

    auto wrapper_name = "__c2pnk_ffi_wrapper_" + func_decl->getName();
    bool is_void_return = info.return_info.original_return_type->isVoidType();
    auto return_type = info.return_info.original_return_type;

    // build the 4 wrapper call arguments up front
    std::array<std::string, 4> slot_args = {"0UL", "0UL", "0UL", "0UL"};
    for (size_t slot = 0; slot < 4; slot++) {
      if (!info.param_infos.at(slot).has_value()) {
        continue;
      }
      auto &param_info = *info.param_infos.at(slot);
      const auto *arg_expr = call_expr->getArg(static_cast<unsigned>(param_info.original_param_index));

      auto src = GetSourceText(arg_expr, data.Ctx);
      if (auto error = src.takeError()) {
        return llvm::joinErrors(
            CreateRuntimeError(
                llvm::formatv("\n    at {0}\nCouldn't get source text for argument {1} of call to FFI function {2}",
                              call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                              param_info.original_param_index, func_decl->getName())),
            std::move(error));
      }
      slot_args.at(slot) = llvm::formatv("(uintptr_t)({0})", *src).str();
    }

    // optimization: void return + no non-fastpath params means no locals and no buffer are
    // needed at all, so a plain call expression suffices
    if (is_void_return && !info.HasInputSlot()) {
      os << llvm::formatv("{0}({1}, {2}, {3}, {4})", wrapper_name, slot_args[0], slot_args[1], slot_args[2],
                          slot_args[3]);
      os.flush();
      return llvm::Error::success();
    }

    os << "({\n";

    // local buffer + fill-in for whatever params didn't fit a fastpath slot
    if (info.HasInputSlot()) {
      for (auto &param_info : info.non_fast_path_param_infos) {
        auto [offset, size] = *param_info.offset_and_size;
        auto param_type = param_info.original_param_decl->getType();
        auto index = param_info.original_param_index;
        const auto *arg_i = call_expr->getArg(static_cast<unsigned>(index))->IgnoreParenImpCasts();
        auto arg_i_src = GetSourceText(arg_i, data.Ctx);
        if (auto error = arg_i_src.takeError()) {
          return llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                      "\n    at {0}\nFailed to get source text for argument {1} of CallExpr",
                                      arg_i->getBeginLoc().printToString(data.Ctx.getSourceManager()), index))),
                                  std::move(error));
        }

        if (!func_decl->isVariadic()) {
          if (!arg_i->isLValue()) {
            // requires tmp var to hold the value of the argument so we can take the address of it
            std::string tmp_var_name = GetTempVarName(llvm::formatv("arg_{0}", index));
            os << llvm::formatv("{0} = {1};\n", PrintType(param_type, tmp_var_name), *arg_i_src);
            os << llvm::formatv("__c2pnk_memcpy(__c2pnk_ffi_input_buf_{0} + {1}UL, (uint8_t *)&{2}, {3}UL);\n",
                                func_name, offset, tmp_var_name, size);
          } else {
            os << llvm::formatv("__c2pnk_memcpy(__c2pnk_ffi_input_buf_{0} + {1}UL, (uint8_t *)&{2}, {3}UL);\n",
                                func_name, offset, *arg_i_src, size);
          }
        }
      }
    }

    if (auto idx = info.OutputBufSlotIndex()) {
      slot_args.at(*idx) = llvm::formatv("(uintptr_t)&__c2pnk_ffi_output_buf_{0}", func_name);
    }
    if (auto idx = info.InputBufSlotIndex()) {
      slot_args.at(*idx) = llvm::formatv("(uintptr_t)__c2pnk_ffi_input_buf_{0}", func_name);
    }

    os << llvm::formatv("{0}({1}, {2}, {3}, {4});\n", wrapper_name, slot_args[0], slot_args[1], slot_args[2],
                        slot_args[3]);
    os.flush();

    if (!is_void_return) {
      auto return_type_pointer = data.Ctx.getPointerType(return_type).getAsString(policy);
      os << llvm::formatv("*({0})__c2pnk_ffi_output_buf_{1};\n", return_type_pointer, func_name);
    }
    os << "})";
    os.flush();

    return llvm::Error::success();
  }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    auto *tmp_function_decl = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto cleanup =
        llvm::scope_exit([this, tmp_function_decl] -> void { data.current_function_decl = tmp_function_decl; });

    if (!RecursiveASTVisitor<Worker>::TraverseFunctionDecl(func_decl)) {
      return false;
    }

    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(func_decl->getBeginLoc()))) {
      return true;
    }

    auto it = data.function_map.find(func_decl);
    if (func_decl->isThisDeclarationADefinition()) {
      if (it == data.function_map.end()) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nFunctionDecl {1} not found in function_map",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
        return false;
      }
    } else {
      // get def of the function decl
      auto *def_decl = func_decl->getDefinition();
      if (def_decl == nullptr) {
        // probably an ffi function
        return true;
      }

      it = data.function_map.find(def_decl);
      if (it == data.function_map.end()) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nFunctionDecl real def {1} not found in function_map",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
        return false;
      }
    }

    auto function_name = func_decl->getName();
    if (function_name.starts_with("__c2pnk_") || function_name == "main") {
      // don't rewrite calls to pancake helper functions
      return true;
    }

    bool is_external_entry_point = false;
    for (const auto *attribute : func_decl->attrs()) {
      if (const auto *annotation_attr = llvm::dyn_cast<clang::AnnotateAttr>(attribute)) {
        if (annotation_attr->getAnnotation() == "__c2pnk_rewritten_non_ffi_function") {
          return true;
        }

        if (annotation_attr->getAnnotation() == "__c2pnk_external_entry_point") {
          is_external_entry_point = true;
        }
      }
    }

    auto &function_info = it->second;
    if (!function_info.needs_rewriting && !function_info.turn_return_from_void_to_uint) {
      return true;
    }

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

      // add annotations
      os << "[[clang::annotate(\"__c2pnk_rewritten_non_ffi_function\")]]\n";
      for (const auto *attribute : func_decl->attrs()) {
        if (const auto *annotation_attr = llvm::dyn_cast<clang::AnnotateAttr>(attribute)) {
          if (annotation_attr->getAnnotation() == "__c2pnk_external_entry_point") {
            // DON'T print it for this function because the name has been changed
            // we instead print it for the new "wrapper" function inserted AFTER this modified function definition!
            continue;
          }
        }
        attribute->printPretty(os, data.Ctx.getPrintingPolicy());
        os << "\n";
      }

      // function specifiers
      os << GetStorageClassSpecifierString(func_decl->getStorageClass()) << " ";
      if (func_decl->isInlineSpecified()) {
        os << "inline ";
      }

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
      if (is_external_entry_point) {
        os << "__c2pnk_external_entry_point_";
      }
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
      // note begin loc DOESn't include the attributes so the external entry point annotation will be duplicated.
      // the real way is to use the name of the func to decide during the final pass
      auto begin_loc = func_decl->getBeginLoc();
      auto l_brace = func_decl->getBody()->getBeginLoc(); // points at '{'
      auto after_l_brace = Lexer::getLocForEndOfToken(l_brace, 0, sm, data.Ctx.getLangOpts());
      auto range = CharSourceRange::getCharRange(begin_loc, after_l_brace);
      if (auto error =
              data.non_ffi_rewrites[func_decl].add(Replacement(sm, range, replacement_text, data.Ctx.getLangOpts()))) {
        data.error = CreateRuntimeError(
            llvm::formatv("\n    at {0}\nNon-ffi function definition rewrite for function {1} FAILED",
                          func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
        return false;
      }
      os.flush();

      if (is_external_entry_point) {
        // we renamed the function earlier to prepend __c2pnk_external_entry_point_ to the name
        //
        // Now we want to add a new function with the original name that calls the renamed function.

        std::string new_func_text;
        llvm::raw_string_ostream new_func_os(new_func_text);

        // add annotations
        new_func_os << "\n\n[[clang::annotate(\"__c2pnk_external_entry_point\")]]\n";
        new_func_os << "[[clang::annotate(\"__c2pnk_rewritten_non_ffi_function\")]]\n";

        // grab the signature part of the original definition
        auto before_l_brace = l_brace.getLocWithOffset(-1);
        auto func_sig =
            Lexer::getSourceText(CharSourceRange::getCharRange(begin_loc, before_l_brace), sm, data.Ctx.getLangOpts());
        new_func_os << func_sig << " {\n";

        // add a call to the renamed function
        std::string call_text;
        llvm::raw_string_ostream call_os(call_text);
        call_os << "__c2pnk_external_entry_point_" << function_name << "(";

        // forward the args
        for (const auto &param_info : function_info.param_infos) {
          if (param_info.hoisted_name.has_value()) {
            // this is funadmentally incompatible!
            data.error = CreateRuntimeError(
                std::move(llvm::formatv("\n    at {0}\nExternal entry point function {1} needs a hoisted parameter "
                                        "{2}, which is not supported",
                                        func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()),
                                        func_decl->getName(), param_info.param_decl->getName())));
            return false;
          }

          if (!param_info.param_decl->getType()->isVoidType()) {
            call_os << param_info.param_decl->getName() << ", ";
          }
        }
        call_os.flush();
        if (call_text.ends_with(", ")) {
          call_text.resize(call_text.size() - 2);
        }
        call_os << ")";
        call_os.flush();

        if (function_info.turn_return_from_void_to_uint) {
          // don't add a return value since the original function returns void
          new_func_os << call_text << ";\n";
        } else {
          new_func_os << "  return " << call_text << ";\n";
        }

        // end the function
        new_func_os << "}\n";
        new_func_os.flush();

        // add the new function AFTER the original function definition
        auto after_r_brace =
            Lexer::getLocForEndOfToken(func_decl->getBody()->getEndLoc(), 0, sm, data.Ctx.getLangOpts());
        // bit of a hack putting it in the return rewrites...
        if (auto error = data.non_ffi_rewrites[func_decl].add(Replacement(sm, after_r_brace, 0, new_func_text))) {
          data.error = llvm::joinErrors(
              CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nFailed to add non-ffi function definition rewrite for function {1}",
                  func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()))),
              std::move(error));
          return false;
        }
      }

      if (function_info.turn_return_from_void_to_uint) {
        // insert a return 0UL at the end of the function right before the closing brace
        auto r_brace = func_decl->getBody()->getEndLoc(); // location of '}'
        auto range = CharSourceRange::getCharRange(r_brace, r_brace);
        if (auto error =
                data.non_ffi_rewrites[func_decl].add(Replacement(sm, range, "return 0UL;\n", data.Ctx.getLangOpts()))) {
          data.error = CreateRuntimeError(
              llvm::formatv("\n    at {0}\nNon-ffi function definition {1} already has a return rewrite",
                            func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()));
          return false;
        }
      }
    } else {
      // non-ffi function decl

      // don't touch the function signature at all.
      if (is_external_entry_point) {
        return RecursiveASTVisitor<Worker>::TraverseFunctionDecl(func_decl);
      }

      // attributes
      for (const auto *attribute : func_decl->attrs()) {
        attribute->printPretty(os, data.Ctx.getPrintingPolicy());
        os << "\n";
      }

      // function specifiers
      os << GetStorageClassSpecifierString(func_decl->getStorageClass()) << " ";
      if (func_decl->isInlineSpecified()) {
        os << "inline ";
      }

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
      // add the replacement under the func definition's decl!
      if (auto error = data.non_ffi_rewrites[func_decl->getDefinition()].add(
              {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(func_decl->getSourceRange()),
               replacement_text, data.Ctx.getLangOpts()})) {
        data.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to add non-ffi function declaration rewrite for function {1}",
                func_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), func_decl->getName()))),
            std::move(error));
        return false;
      };
    }

    return true;
  }

  auto TraverseCallExpr(CallExpr *call_expr) -> bool {
    if (!RecursiveASTVisitor<Worker>::TraverseCallExpr(call_expr)) {
      return false;
    }

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
      data.error = CreateRuntimeError(
          std::move(llvm::formatv("\n    at {0}\nCallExpr has no direct callee",
                                  call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()))));
      return false;
    }

    auto callee_name = callee_decl->getName();
    if (callee_name.starts_with("__c2pnk_") || callee_name == "main") {
      // don't rewrite calls to pancake helper functions
      return true;
    }

    if (data.current_function_decl != nullptr) {
      auto current_function_name = data.current_function_decl->getName();
      if (current_function_name.starts_with("__c2pnk_")) {
        // don't rewrite calls to pancake helper functions
        return true;
      }
    }

    for (const auto *attribute : callee_decl->attrs()) {
      if (const auto *annotation_attr = llvm::dyn_cast<clang::AnnotateAttr>(attribute)) {
        if (annotation_attr->getAnnotation() == "__c2pnk_rewritten_non_ffi_function") {
          return true;
        }
      }
    }

    auto it = data.function_map.find(callee_decl);
    if (it != data.function_map.end()) {
      // non-ffi function call

      auto &function_info = it->second;
      if (!function_info.needs_rewriting && function_info.turn_return_from_void_to_uint) {
        // optimization to prevent a gnu statement expression from being generated if only the return type changes
        // from void -> uint32/64_t
        std::string expanded;
        llvm::raw_string_ostream os(expanded);
        os << "(void)" << callee_name << "(";
        for (const auto &[i, param_info] : function_info.param_infos | std::views::enumerate) {
          auto *arg_i = call_expr->getArg(static_cast<unsigned>(i));
          auto arg_i_src = GetSourceText(arg_i, data.Ctx);
          if (auto error = arg_i_src.takeError()) {
            data.error = llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                              "\n    at {0}\nFailed to get source text for argument {1} of CallExpr",
                                              arg_i->getBeginLoc().printToString(data.Ctx.getSourceManager()), i))),
                                          std::move(error));
            return false;
          }
          os << *arg_i_src << ", ";
        }
        os.flush();
        if (expanded.ends_with(", ")) {
          expanded.resize(expanded.size() - 2);
        }
        os << ")";
        os.flush();
        if (auto error = data.non_ffi_rewrites[callee_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(call_expr->getSourceRange()), expanded,
                 data.Ctx.getLangOpts()})) {
          data.error = llvm::joinErrors(
              CreateRuntimeError(
                  std::move(llvm::formatv("\n    at {0}\nFailed to add rewrite for non-FFI function call\n    Did you "
                                          "recursively call this function? If so, c2pancake does not support that yet.",
                                          call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
              std::move(error));
          return false;
        }
      } else if (function_info.needs_rewriting) {
        // replace with a gnu statement expression that loads the hoisted parameters and return value
        std::string expanded;
        llvm::raw_string_ostream os(expanded);
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

        for (const auto &[i, param_info] : function_info.param_infos | std::views::enumerate) {
          auto *arg_i = call_expr->getArg(static_cast<unsigned>(i));
          auto arg_i_src = GetSourceText(arg_i, data.Ctx);
          if (auto error = arg_i_src.takeError()) {
            data.error = llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                              "\n    at {0}\nFailed to get source text for argument {1} of CallExpr",
                                              arg_i->getBeginLoc().printToString(data.Ctx.getSourceManager()), i))),
                                          std::move(error));
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

        if (auto error = data.non_ffi_rewrites[callee_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(call_expr->getSourceRange()), expanded,
                 data.Ctx.getLangOpts()})) {
          data.error = llvm::joinErrors(
              CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nFailed to add rewrite for non-FFI function call: {1}\n    Did you recursively call "
                  "this function? If so, c2pancake does not support that yet.",
                  call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()))),
              std::move(error));
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
        return 0UL;
      }

      static uint8_t __c2pnk_ffi_example_func_input_buf[CALCULATED_SIZE];
      static uint8_t __c2pnk_ffi_example_func_output_buf[CALCULATED_SIZE];

      // actual call expression gets rewritten to the following. note all the args have to be located on the heap!
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf, &arg1, sizeof(arg1));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, (uint8_t *)&arg2, sizeof(arg2));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, (uint8_t *)&arg3, sizeof(arg3));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, (uint8_t *)&arg4, sizeof(arg4));
      __c2pnk_memcpy(__c2pnk_ffi_example_func_input_buf + CALCULATED_OFFSET, (uint8_t *)&arg5, sizeof(arg5));
      // actual function call
      __c2pnk_ffi_example_func(__c2pnk_ffi_example_func_input_buf, sizeof(__c2pnk_ffi_example_func_input_buf),
      __c2pnk_ffi_example_func_output_buf, sizeof(__c2pnk_ffi_example_func_output_buf));
      __c2pnk_memcpy(&ret_val, __c2pnk_ffi_example_func_output_buf, sizeof(ret_val));

      // the function below will be located in the ffi_stubs file!!!
      uint64_t __c2pnk_ffi_example_func(uint8_t *input_buf, uint64_t input_buf_len, uint8_t *output_buf, uint64_t
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
        return 0UL;
      }
      */
      auto *it = data.ffi_rewrites.find(callee_decl);
      if (callee_decl->isVariadic()) {
        // TODO variadic functions...
        // create a new name mangled variadic version of the function every time...
      } else if (it == data.ffi_rewrites.end()) {
        // create ffi "stub" for this function
        auto wrapper_info = ConstructFunctionWrapperInfo(callee_decl);
        data.ffi_function_info.insert({callee_decl, FFIFunctionWrapperInfo(wrapper_info)});

        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);
        if (auto error = GenerateFFIWrapperFunction(callee_decl, os)) {
          data.error = llvm::joinErrors(
              CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nFailed to generate FFI wrapper function: {1}",
                  callee_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()))),
              std::move(error));
          return false;
        }

        // add to the top of the main source file
        if (auto error = data.ffi_rewrites[callee_decl].add(
                {data.Ctx.getSourceManager(), sm.getLocForStartOfFile(sm.getMainFileID()), 0, replacement_text})) {
          data.error = llvm::joinErrors(
              CreateRuntimeError(std::move(llvm::formatv(
                  "\n    at {0}\nFailed to add rewrite for FFI function wrapper: {1}",
                  callee_decl->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()))),
              std::move(error));
          return false;
        }
      }

      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      if (auto error = CallExprToFFIWrapperStmtExpr(call_expr, callee_decl, os)) {
        data.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to generate FFI wrapper call for function: {1}",
                call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()))),
            std::move(error));
        return false;
      }

      if (auto error = data.ffi_rewrites[callee_decl].add({data.Ctx.getSourceManager(),
                                                           CharSourceRange::getTokenRange(call_expr->getSourceRange()),
                                                           replacement_text, data.Ctx.getLangOpts()})) {
        data.error = llvm::joinErrors(
            CreateRuntimeError(std::move(llvm::formatv(
                "\n    at {0}\nFailed to add rewrite for FFI function call: {1}\n    Did you "
                "recursively call this function? If so, c2pancake does not support that yet.",
                call_expr->getBeginLoc().printToString(data.Ctx.getSourceManager()), callee_decl->getName()))),
            std::move(error));
        return false;
      }
    }

    return true;
  };

  static auto ValidateReturnStmt(ReturnStmt *return_stmt) -> bool {
    if (return_stmt->getRetValue() == nullptr) {
      return true;
    }

    if (auto *int_lit = llvm::dyn_cast<IntegerLiteral>(return_stmt->getRetValue())) {
      if (int_lit->getValue() == 0) {
        return false;
      }
    }

    return true;
  }

  auto TraverseReturnStmt(ReturnStmt *return_stmt) -> bool {
    if (!RecursiveASTVisitor<Worker>::TraverseReturnStmt(return_stmt)) {
      return false;
    }

    if (data.error) {
      return false;
    }

    auto &sm = data.Ctx.getSourceManager();
    if (!sm.isInMainFile(sm.getSpellingLoc(return_stmt->getBeginLoc()))) {
      return true;
    }

    if (data.current_function_decl == nullptr) {
      data.error = CreateRuntimeError(std::move(llvm::formatv(
          "\n    at {0}\nReturnStmt found outside of a FunctionDecl", return_stmt->getBeginLoc().printToString(sm))));
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
            data.error = llvm::joinErrors(CreateRuntimeError(std::move(llvm::formatv(
                                              "\n    at {0}\nFailed to get source text for ReturnStmt",
                                              return_stmt->getBeginLoc().printToString(data.Ctx.getSourceManager())))),
                                          std::move(error));
            return false;
          }
          os << *ret_value_src;
        }
        os << ";\n";
        // return 0UL instead as per pancake rules
        os << "return 0UL;\n";
        os << "}";
        os.flush();

        if (auto error = data.non_ffi_rewrites[data.current_function_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(return_stmt->getSourceRange()),
                 replacement_text, data.Ctx.getLangOpts()})) {
          data.error = std::move(error);
          return false;
        };
      }
    } else if (function_info.turn_return_from_void_to_uint) {
      if (ValidateReturnStmt(return_stmt)) {
        // rewrite the return statement to return 0UL instead as per pancake rules
        std::string replacement_text;
        llvm::raw_string_ostream os(replacement_text);
        os << "return 0UL";
        os.flush();

        if (auto error = data.non_ffi_rewrites[data.current_function_decl].add(
                {data.Ctx.getSourceManager(), CharSourceRange::getTokenRange(return_stmt->getSourceRange()),
                 replacement_text, data.Ctx.getLangOpts()})) {
          data.error = std::move(error);
          return false;
        };
      }
    }

    return true;
  }
};
} // namespace RewriteFunctions
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  CollectFunctionInfo::WorkerData::FunctionMap function_map;
  {
    CollectFunctionInfo::WorkerData data{.Ctx = Ctx, .ps_ctx = ps_ctx, .function_map = function_map};
    CollectFunctionInfo::Worker w(data);
    w.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (data.error) {
      ps_ctx.error = std::move(data.error);
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  NonFFIFunctionRewrites non_ffi_rewrites;
  FFIFunctionRewrites ffi_rewrites;
  FFIVariadicFunctionRewrites ffi_variadic_rewrites;
  {
    RewriteFunctions::WorkerData data(Ctx, ps_ctx, function_map, non_ffi_rewrites, ffi_rewrites, ffi_variadic_rewrites);
    RewriteFunctions::Worker w(data);
    w.TraverseDecl(Ctx.getTranslationUnitDecl());

    if (data.error) {
      ps_ctx.error = std::move(data.error);
      ps_ctx.whats_next = WhatsNext::MoveToNextFile;
      return;
    }
  }

  llvm::SmallVector<Replacements> merged_groups;
  merged_groups.reserve(non_ffi_rewrites.size() + ffi_rewrites.size() + ffi_variadic_rewrites.size());
  auto append_values = [&merged_groups](auto &map) -> auto {
    auto values = llvm::make_second_range(map);
    merged_groups.insert(merged_groups.end(), values.begin(), values.end());
  };
  append_values(ffi_rewrites);
  append_values(ffi_variadic_rewrites);
  if (merged_groups.empty()) {
    append_values(non_ffi_rewrites);
  } else {
    // we want to apply ALL ffi rewrites first, AND THEN apply the non-ffi rewrites,
    // otherwise the non-ffi rewrites might obscure the conditions required to apply ffi rewrites.
    ps_ctx.whats_next = WhatsNext::RepeatPass;
  }

  std::ranges::sort(merged_groups, [](const auto &a, const auto &b) -> auto {
    return a.begin()->getOffset() < b.begin()->getOffset();
  });

  Replacements merged_output;
  auto all_ok = true;
  for (const auto &group : merged_groups) {
    auto ok = true;
    auto attempt = merged_output;
    for (const auto &r : group) {
      if (auto error = attempt.add(r)) {
        llvm::consumeError(std::move(error));
        ok = false;
        all_ok = false;
        break;
      }
    }
    if (ok) {
      merged_output = std::move(attempt); // commit the whole group}
    }
  }

  ps_ctx.replacements = std::move(merged_output);
  if (!ps_ctx.whats_next.has_value()) {
    if (all_ok) {
      ps_ctx.whats_next = WhatsNext::MoveToNextPass;
    } else {
      ps_ctx.whats_next = WhatsNext::RepeatPass;
    }
  }
}
} // namespace pancake::pass_function_calling
