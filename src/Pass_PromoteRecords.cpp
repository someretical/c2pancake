#include "Pass_PromoteRecords.h"
#include "Utils.h"

#include <clang-tools-extra/clangd/FindTarget.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>
#include <clang/Tooling/Refactoring/AtomicChange.h>
#include <clang/Tooling/Refactoring/Rename/USRFindingAction.h>
#include <clang/Tooling/Transformer/RangeSelector.h>
#include <clang/Tooling/Transformer/RewriteRule.h>
#include <clang/Tooling/Transformer/Stencil.h>
#include <clang/Tooling/Transformer/Transformer.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <string>
#include <utility>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::clangd;
using namespace clang::transformer;
using namespace clang::tooling;

namespace pancake::pass_name_anon_records {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(recordDecl(isDefinition(), isExpansionInMainFile()).bind("record"),
                  [](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
                    const auto *rd = result.Nodes.getNodeAs<RecordDecl>("record");

                    /*
                    Cases

                    DO name
                    struct { int x; } a;

                    DO name
                    typedef struct { int x; } T;

                    DON'T name since x is injected as an IndirectFieldDecl into Outer
                    struct Outer {
                        struct {
                            int x;
                        };
                    };

                    DON'T name since x is injected as an IndirectFieldDecl into U
                    union U {
                        struct {
                            int x;
                            int y;
                        };
                        int z;
                    };
                    */
                    if (!rd->getIdentifier() && !rd->isAnonymousStructOrUnion() &&
                        result.Context->getSourceManager().isInMainFile(rd->getBeginLoc())) {
                      FullSourceLoc const loc(rd->getBeginLoc(), *result.SourceManager);
                      auto identifier = llvm::formatv("__c2pnk_anon_record_L{0}C{1}", loc.getSpellingLineNumber(),
                                                      loc.getSpellingColumnNumber());
                      std::string replacement_text;
                      llvm::raw_string_ostream os(replacement_text);
                      os << llvm::formatv("{0} {1} ", rd->isUnion() ? "union" : "struct", identifier);

                      SourceLocation const l_brace = rd->getBraceRange().getBegin();
                      SourceLocation const r_brace = rd->getBraceRange().getEnd();
                      PrintSourceText(os, CharSourceRange::getTokenRange(l_brace, r_brace), *result.Context);
                      os.flush();

                      return edit(changeTo(node("record"), cat(replacement_text)))(result);
                    }

                    return noEdits()(result);
                  });
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
  auto t = Transformer(MakeRule(), [&changes](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c)
      changes.insert(changes.end(), c->begin(), c->end());
    else
      llvm::consumeError(c.takeError());
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  bool add_error_occurred = false;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
        add_error_occurred = true;
      }
    }
  }

  pa_ctx.run_result = RunResult::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.run_result = RunResult::Success;
  }
}
}; // namespace pancake::pass_name_anon_records

namespace pancake::pass_rename_to_be_promoted_records {
namespace {
auto MakeRule() -> RewriteRule {
  return makeRule(recordDecl(isDefinition(), isExpansionInMainFile(), unless(hasDeclContext(translationUnitDecl())),
                             unless(hasAncestor(recordDecl())))
                      .bind("record"),
                  [](const MatchFinder::MatchResult &result) -> Expected<SmallVector<Edit, 1>> {
                    const auto *rd = result.Nodes.getNodeAs<RecordDecl>("record");

                    auto name = rd->getName();
                    assert(!name.empty() && "RecordDecl should have a name at this point");
                    const char *prefix = "__c2pnk_promoted_record";
                    if (name.starts_with(prefix))
                      return noEdits()(result);

                    FullSourceLoc const loc(rd->getBeginLoc(), *result.SourceManager);
                    auto new_identifier = llvm::formatv("{0}_{1}_L{2}C{3}", prefix, name, loc.getSpellingLineNumber(),
                                                        loc.getSpellingColumnNumber())
                                              .str();

                    // // The problem with the below approach is that getOccurrencesOfUSRs only works reliably for
                    // // the global scope...
                    // auto usrs = getUSRsForDeclaration(rd, *result.Context);
                    // auto occurences = getOccurrencesOfUSRs(usrs, name, result.Context->getTranslationUnitDecl());
                    // const auto total = std::accumulate(
                    //     occurences.begin(), occurences.end(), size_t{0},
                    //     [](const auto &total, const SymbolOccurrence &b) -> auto { return total +
                    //     b.getNameRanges().size(); });

                    // llvm::outs() << llvm::formatv("Renaming record {0} to {1}, {2} occurences\n", name,
                    // new_identifier, total);

                    // llvm::SmallVector<Edit, 1> edits;
                    // edits.reserve(total);
                    // for (const auto &occ : occurences) {
                    //   llvm::outs() << "Occurrences: " << occ.getNameRanges().size() << "\n";
                    //   for (const auto &range : occ.getNameRanges()) {
                    //     llvm::outs() << llvm::formatv("  Occurrence at {0}\n",
                    //                                   range.getBegin().printToString(*result.SourceManager));
                    //     edits.push_back(Edit{.Kind = EditKind::Range,
                    //                          .Range = CharSourceRange::getTokenRange(range),
                    //                          .Replacement = new_identifier,
                    //                          .Note = "Promoting record to top-level"});
                    //   }
                    // }

                    // Instead we use an arguably even more fucked approach.
                    // We use clangd's internal functions for renaming symbols which works but is really brittle
                    // across updates...
                    llvm::SmallVector<Edit, 16> edits;
                    const auto *canonical_target = rd->getCanonicalDecl();

                    findExplicitReferences(
                        *result.Context,
                        [&](const ReferenceLoc &ref) -> void {
                          for (const auto *target : ref.Targets) {
                            if (const auto *target_rd = dyn_cast<RecordDecl>(target)) {
                              if (target_rd->getCanonicalDecl() == canonical_target) {
                                edits.push_back(Edit{.Kind = EditKind::Range,
                                                     .Range = CharSourceRange::getTokenRange(ref.NameLoc),
                                                     .Replacement = new_identifier,
                                                     .Note = "Promoting record to top-level"});
                              }
                            }
                          }
                        },
                        nullptr);

                    return edits;
                  });
}
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<AtomicChange, 64> changes;
  auto t = Transformer(MakeRule(), [&changes](llvm::Expected<llvm::MutableArrayRef<AtomicChange>> c) -> void {
    if (c)
      changes.insert(changes.end(), c->begin(), c->end());
    else
      llvm::consumeError(c.takeError());
  });

  MatchFinder finder;
  t.registerMatchers(&finder);
  finder.matchAST(Ctx);

  bool add_error_occurred = false;
  for (const auto &change : changes) {
    for (const auto &r : change.getReplacements()) {
      if (auto err = pa_ctx.replacements.add(r)) {
        llvm::consumeError(std::move(err));
        llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
        add_error_occurred = true;
      }
    }
  }

  pa_ctx.run_result = RunResult::RepeatPass;
  if (!add_error_occurred && changes.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.run_result = RunResult::Success;
  }
}
} // namespace pancake::pass_rename_to_be_promoted_records

namespace pancake::pass_promote_records {
namespace {
struct WorkerData {
  ASTContext &Ctx;
  PipelineActionCtx &pa_ctx;
  llvm::SmallVector<Replacement, 64> &replacements;
  FunctionDecl *current_function_decl = nullptr;
  RecordDecl *current_record_decl = nullptr;
  llvm::DenseMap<FunctionDecl *, llvm::SmallVector<RecordDecl *, 16>> function_to_record_decls;
};

class Worker : public RecursiveASTVisitor<Worker> {
  struct WorkerData &data;

public:
  explicit Worker(struct WorkerData &data) : data(data) {}

  // process all outer record decls first
  static auto shouldTraversePostOrder() -> bool { return false; }

  auto TraverseFunctionDecl(FunctionDecl *func_decl) -> bool {
    auto *tmp = data.current_function_decl;
    data.current_function_decl = func_decl;
    auto res = RecursiveASTVisitor::TraverseFunctionDecl(func_decl);

    auto it = data.function_to_record_decls.find(func_decl);
    if (it != data.function_to_record_decls.end()) {
      std::string replacement_text;
      llvm::raw_string_ostream os(replacement_text);
      os << "\n/* c2pancake: promoted record declarations for function " << func_decl->getName() << " BEGIN */\n";
      for (const auto *record_decl : it->second) {
        PrintSourceText(os, record_decl, data.Ctx);
        os << ";\n";
      }
      os << "/* c2pancake: promoted record declarations for function " << func_decl->getName() << " END */\n";
      os.flush();
      // insert before start of function...
      data.replacements.emplace_back(data.Ctx.getSourceManager(), func_decl->getBeginLoc(), 0, replacement_text);
    }

    data.current_function_decl = tmp;
    return res;
  }

  auto TraverseRecordDecl(RecordDecl *record_decl) -> bool {
    auto *tmp = data.current_record_decl;
    data.current_record_decl = record_decl;
    auto res = RecursiveASTVisitor::TraverseRecordDecl(record_decl);
    data.current_record_decl = tmp;
    return res;
  }

  auto TraverseDeclStmt(DeclStmt *declStmt) -> bool {
    auto &sm = data.Ctx.getSourceManager();
    if (declStmt == nullptr || !sm.isInMainFile(sm.getSpellingLoc(declStmt->getBeginLoc())) ||
        data.current_function_decl == nullptr || data.current_record_decl != nullptr) {
      return true;
    }

    assert(declStmt->isSingleDecl() && "DeclStmt should only have a single decl at this point");
    if (auto *record_decl = dyn_cast<RecordDecl>(declStmt->getSingleDecl())) {
      if (record_decl->isThisDeclarationADefinition()) {
        auto name = record_decl->getName();
        assert(name.starts_with("__c2pnk_promoted_record") && "RecordDecl should have a promoted name at this point");

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        data.function_to_record_decls[data.current_function_decl].push_back(record_decl);
        data.replacements.emplace_back(data.Ctx.getSourceManager(),
                                       CharSourceRange::getTokenRange(declStmt->getSourceRange()), "",
                                       data.Ctx.getLangOpts());
      }
    }

    return RecursiveASTVisitor::TraverseDeclStmt(declStmt);
  }
};
} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  llvm::SmallVector<Replacement, 64> replacements;
  WorkerData data{.Ctx = Ctx,
                  .pa_ctx = pa_ctx,
                  .replacements = replacements,
                  .current_function_decl = nullptr,
                  .current_record_decl = nullptr,
                  .function_to_record_decls = llvm::DenseMap<FunctionDecl *, llvm::SmallVector<RecordDecl *, 16>>()};
  Worker w(data);
  w.TraverseDecl(Ctx.getTranslationUnitDecl());

  bool add_error_occurred = false;
  for (const auto &r : replacements) {
    if (auto err = pa_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
      add_error_occurred = true;
    }
  }

  pa_ctx.run_result = RunResult::Success;
  if (!add_error_occurred && replacements.empty()) {
    // All edits successfully added; no need to repeat this pass
    pa_ctx.run_result = RunResult::Success;
  }
}
} // namespace pancake::pass_promote_records
