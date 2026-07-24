#include "Pass_InjectHeaders.h"
#include "Utils.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecordLayout.h>
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
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <utility>

using namespace clang;
using namespace clang::tooling;

namespace pancake::pass_inject_headers {
auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {
  const tooling::IncludeStyle style{};
  const auto &sm = Ctx.getSourceManager();
  const auto *file_entry = sm.getFileEntryForID(sm.getMainFileID());
  assert(file_entry != nullptr && "Main file entry should not be null");
  const auto &file_name = file_entry->tryGetRealPathName();
  const auto source_text = sm.getBufferOrFake(sm.getMainFileID()).getBuffer();
  const HeaderIncludes includes(file_name, source_text, style);

  llvm::SmallVector<Replacement, 16> replacements;

  // can be called repeatedly
  auto res = includes.insert("stdint.h", true, IncludeDirective::Include);
  if (res.has_value()) {
    replacements.push_back(res.value());
  } else {
    // the header already exists
  }

  bool add_error_occurred = false;
  for (const auto &r : replacements) {
    if (auto err = pa_ctx.replacements.add(r)) {
      llvm::consumeError(std::move(err));
      llvm::errs() << llvm::formatv("{0} Add replacement conflict, retrying next pass...\n", LogBegin(pa_ctx));
      add_error_occurred = true;
    }
  }
  assert(!add_error_occurred && "Failed to add header replacement");
}
} // namespace pancake::pass_inject_headers
