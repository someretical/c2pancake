#include "Pass_SwitchToIf.h"

#include <cassert>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/Core/Replacement.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace pancake::normalise_switches {
namespace {} // namespace

auto Consumer::HandleTranslationUnit(ASTContext &Ctx) -> void {

  // pa_ctx.failure_mode = FailureMode::Repeat;
  // if (!add_error_occurred && changes.empty()) {
  //   // All edits successfully added; no need to repeat this pass
  //   pa_ctx.failure_mode = FailureMode::None;
  // }
}

} // namespace pancake::normalise_switches
