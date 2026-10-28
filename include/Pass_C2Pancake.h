#ifndef C2PANCAKE_PASS_C2PANCAKE_H
#define C2PANCAKE_PASS_C2PANCAKE_H

#include "Pipeline.h"
#include "Utils.h"

#include <clang/AST/Decl.h>

namespace pancake::pass_c2pancake {
struct Finalizer {
  static auto Write(StageContext &ctx, clang::CompilerInstance &compiler) -> llvm::Error;
};

struct TranslationUnitOutput {
  std::string content;
};
struct FFIOutput {
  std::string content;
};
struct HeapSize {
  uint64_t size;
};
struct GlobalVarData {
  std::string type;
  std::string name;
  uint64_t addr;
  bool is_external;
};

inline constexpr pancake::ArtifactKey<TranslationUnitOutput> translation_unit_output_key{
    "C2Pancake_translation_unit_output"};
inline constexpr pancake::ArtifactKey<FFIOutput> ffi_output_key{"C2Pancake_ffi_output"};
inline constexpr pancake::ArtifactKey<HeapSize> heap_size_key{"C2Pancake_heap_size"};
inline constexpr pancake::ArtifactKey<llvm::DenseMap<clang::VarDecl *, GlobalVarData>> global_var_data_key{
    "C2Pancake_global_var_data"};

class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public ClangStage<Consumer, Finalizer> {
public:
  explicit Action(StageContext &ctx) : ClangStage<Consumer, Finalizer>(ctx) { ctx.stage_name = "C2Pancake"; }
};
} // namespace pancake::pass_c2pancake

#endif // C2PANCAKE_PASS_C2PANCAKE_H
