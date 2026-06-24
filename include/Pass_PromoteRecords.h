#ifndef C2PANCAKE_PASS_PROMOTERECORDS_H
#define C2PANCAKE_PASS_PROMOTERECORDS_H

#include "Pipeline.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>

#include <memory>
#include <string>

namespace pancake::pass_promote_records {

struct FieldPromotion {
  std::string newTypeName; // e.g. "int64_t"
  bool isBitField{false};
  unsigned bitFieldWidth{0}; // original width in bits
  bool sizeDecreased{false};
  unsigned origBytes{0};
};

class PassFind : public clang::RecursiveASTVisitor<PassFind> {
public:
  llvm::DenseMap<const clang::RecordDecl *, const clang::FunctionDecl *> InitialisedStructs;
  size_t hoist_record_decl_counter = 0;

  auto VisitRecordDecl(clang::RecordDecl *RD) -> bool;
};

// Rewrite engine
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
  void AddReplacement(clang::ASTContext &Ctx, clang::SourceRange SR, const std::string &text,
                      bool includeTerminatingSemicolon);

private:
  void InsertAtOffset(llvm::StringRef file, unsigned offset, const std::string &text);
  void ProcessStruct(const clang::RecordDecl *RD, clang::ASTContext &Ctx);
  void RewriteTypeUses(const clang::RecordDecl *RD, const std::string &newName, clang::ASTContext &Ctx);
  void EmitToFile(const std::string &text) const;

  friend struct Renamer;
};

// frontend
class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "PromoteRecords";
    ctx.failure_behaviour = FailureBehaviour::MoveToNextFile;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};

} // namespace pancake::pass_promote_records

#endif // C2PANCAKE_PASS_PROMOTERECORDS_H