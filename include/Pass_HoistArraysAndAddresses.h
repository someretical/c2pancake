#ifndef C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H
#define C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/OperationKinds.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TypeBase.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace pancake::hoist_arrays_and_addresses {
enum class DeclTreatment : uint8_t {
  // Auto/register local: emit a fresh global decl before the first function,
  // replace the local DeclStmt with initialisation assignments
  NewGlobal,

  // Static local: strip "static", move the whole declaration (with any
  // constant initialiser) to the global block, remove the original DeclStmt
  StaticGlobal,

  // Extern local: the symbol already exists externally; just delete the
  // local re-declaration and rename uses to the bare external name
  ExternRedecl,
};

struct VarHoistEntry {
  std::string newName;
  DeclTreatment treatment;
  const clang::FunctionDecl *func{};
};

struct HoistInfo {
  llvm::DenseMap<const clang::VarDecl *, VarHoistEntry> hoistMap;
};

// Pass 1: Collect all variables that need hoisting
class AddressTakenFinder
    : public clang::RecursiveASTVisitor<AddressTakenFinder> {
public:
  std::set<const clang::VarDecl *> addressTaken;

  auto VisitUnaryOperator(clang::UnaryOperator *UO) -> bool;
};

class CollectVisitor : public clang::RecursiveASTVisitor<CollectVisitor> {
public:
  HoistInfo &info;
  clang::ASTContext &Ctx;

  CollectVisitor(HoistInfo &hi, clang::ASTContext &ctx) : info(hi), Ctx(ctx) {}

  auto VisitFunctionDecl(clang::FunctionDecl *FD) -> bool;

private:
  void walkStmt(clang::Stmt *S, clang::FunctionDecl *FD,
                std::set<const clang::VarDecl *> &addrTaken);
};

// Pass 2: Accumulate edits and apply at the end
class CollectReplacementsVisitor
    : public clang::RecursiveASTVisitor<CollectReplacementsVisitor> {
public:
  clang::tooling::Replacements &Repls;
  clang::ASTContext &Ctx;
  HoistInfo &info;
  clang::SourceManager &SM;

  CollectReplacementsVisitor(clang::tooling::Replacements &repls,
                             clang::ASTContext &ctx, HoistInfo &hi)
      : Repls(repls), Ctx(ctx), info(hi), SM(ctx.getSourceManager()) {}

  // Replace DeclStmts that contain hoisted vars
  auto VisitDeclStmt(clang::DeclStmt *DS) -> bool;

  // Rename every use of a hoisted variable
  auto VisitDeclRefExpr(clang::DeclRefExpr *DR) -> bool;

private:
  // Convert a SourceRange to a tooling::Replacement and add it to the set.
  // tooling::Replacements::add() returns an llvm::Error if there is an
  // irreconcilable conflict; we log and continue rather than crashing.
  void addReplacement(clang::SourceRange range, const std::string &text);
};

class HoistConsumer : public clang::ASTConsumer {
  clang::CompilerInstance &CI;

public:
  std::string current_suffix;
  std::string outputPath;

  explicit HoistConsumer(clang::CompilerInstance &ci) : CI(ci) {}

  void HandleTranslationUnit(clang::ASTContext &Ctx) override;

private:
  void emitToFile(const std::string &text) const;
};

class HoistAction : public clang::ASTFrontendAction {
  std::string cur_suffix_;
  std::string next_suffix_;

public:
  HoistAction(std::string cur_suffix, std::string next_suffix)
      : cur_suffix_(std::move(cur_suffix)),
        next_suffix_(std::move(next_suffix)) {}

  auto CreateASTConsumer(clang::CompilerInstance &CI, clang::StringRef file)
      -> std::unique_ptr<clang::ASTConsumer> override;
};

struct HoistActionFactory : public clang::tooling::FrontendActionFactory {
  std::string cur_suffix;
  std::string next_suffix;
  auto create() -> std::unique_ptr<clang::FrontendAction> override {
    return std::make_unique<HoistAction>(cur_suffix, next_suffix);
  }
};

} // namespace pancake::hoist_arrays_and_addresses

#endif // C2PANCAKE_PASS_HOISTARRAYSANDADDRESSES_H