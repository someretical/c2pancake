#ifndef C2PANCAKE_PASS_PROMOTERECORDS_H
#define C2PANCAKE_PASS_PROMOTERECORDS_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
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
  llvm::DenseMap<const clang::RecordDecl *, const clang::FunctionDecl *>
      InitialisedStructs;
  size_t hoist_record_decl_counter = 0;

  auto VisitRecordDecl(clang::RecordDecl *RD) -> bool;
};

// Rewrite engine
class Consumer : public clang::ASTConsumer {
public:
  std::string current_suffix;
  std::string output_path;

  explicit Consumer(clang::CompilerInstance &CI) : CI(CI) {}

  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
  void addReplacement(clang::ASTContext &Ctx, clang::SourceRange SR,
                      const std::string &text,
                      bool includeTerminatingSemicolon);

private:
  clang::CompilerInstance &CI;
  clang::tooling::Replacements Repls;

  void insertAtOffset(llvm::StringRef file, unsigned offset,
                      const std::string &text);
  void processStruct(const clang::RecordDecl *RD, clang::ASTContext &Ctx);
  void rewriteTypeUses(const clang::RecordDecl *RD, const std::string &newName,
                       clang::ASTContext &Ctx);
  void emitToFile(const std::string &text) const;

  friend struct Renamer;
};

// frontend
class Action : public clang::ASTFrontendAction {
public:
  Action(std::string cur_suffix, std::string next_suffix)
      : cur_suffix_(std::move(cur_suffix)),
        next_suffix_(std::move(next_suffix)) {}

  auto CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file)
      -> std::unique_ptr<clang::ASTConsumer> override;

private:
  std::string cur_suffix_;
  std::string next_suffix_;
};

struct ActionFactory : public clang::tooling::FrontendActionFactory {
  std::string cur_suffix;
  std::string next_suffix;
  auto create() -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<Action>(cur_suffix, next_suffix);
  }
};

} // namespace pancake::pass_promote_records

#endif // C2PANCAKE_PASS_PROMOTERECORDS_H