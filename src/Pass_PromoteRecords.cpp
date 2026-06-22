#include "Pass_PromoteRecords.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeBase.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/LLVM.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Core/Replacement.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

using namespace pancake::pass_promote_records;

namespace {
auto RecordDeclPrefix(bool make_shared, size_t counter, const std::string &func, const std::string &var)
    -> std::string {
  return llvm::formatv("__c2pnk_{0}_record_decl_{1}_{2}_{3}", make_shared ? "shared" : "local", func, var, counter);
}

const bool is32_bit = sizeof(void *) == 4;

auto ComputePromotion(const clang::FieldDecl *F, const clang::ASTContext &Ctx, FieldPromotion &out) -> bool {
  clang::QualType const qt = F->getType();
  const clang::Type *t = qt.getTypePtr();

  // Skip arrays and pointers
  if (t->isArrayType() || t->isPointerType() || t->isReferenceType())
    return false;

  // Skip nested record/union/enum/function/
  if (t->isRecordType())
    return false;

  // Only promote integers and floating-point scalars
  if (!t->isIntegralOrEnumerationType() && !t->isFloatingType())
    return false;

  bool const is_signed = !t->isUnsignedIntegerType();

  auto width = is32_bit ? 32 : 64;
  out.newTypeName = is_signed ? llvm::formatv("int{0}_t", width) : llvm::formatv("uint{0}_t", width);

  if (F->isBitField()) {
    out.isBitField = true;
    out.bitFieldWidth = F->getBitWidthValue();
  }

  uint64_t const orig_bits = Ctx.getTypeSize(qt);
  uint64_t const prom_bits = is32_bit ? 32U : 64U;
  out.origBytes = static_cast<unsigned>(orig_bits / 8);
  if (orig_bits > prom_bits)
    out.sizeDecreased = true;

  return true;
}

auto PrintRecordDecl(const clang::RecordDecl *RD, clang::ASTContext &Ctx, const std::string &overrideName,
                     unsigned indent) -> std::string;

auto PrintFieldDecl(const clang::FieldDecl *F, clang::ASTContext &Ctx, unsigned indent) -> std::string {
  std::string pad(indent, ' ');
  clang::QualType const qt = F->getType();

  // Strip arrays to get at the element type while preserving the array
  // dimensions
  clang::QualType base = qt;
  std::string array_suffix;
  llvm::raw_string_ostream array_suffix_os(array_suffix);
  while (const clang::ArrayType *at = Ctx.getAsArrayType(base)) {
    std::string dim;
    {
      llvm::raw_string_ostream dim_os(dim);
      if (const auto *cat = clang::dyn_cast<clang::ConstantArrayType>(at)) {
        dim_os << llvm::formatv("[{0}]", cat->getSize());
        // NOLINTNEXTLINE(bugprone-branch-clone)
      } else if (clang::isa<clang::IncompleteArrayType>(at)) {
        dim_os << "[]";
      } else {
        // Variable-length array???? not sure how to handle this...
        dim_os << "[]";
      }
      array_suffix_os << dim_os.str() << array_suffix_os.str();
    }
    base = at->getElementType();
  }

  if (const auto *rt = base->getAs<clang::RecordType>()) {
    const clang::RecordDecl *nested = rt->getDecl();
    // handle anonymous nested record
    if (nested->isAnonymousStructOrUnion() || nested->getDeclName().isEmpty()) {
      std::string body = PrintRecordDecl(nested, Ctx, /*overrideName=*/"", indent);
      // body ends with "};" so strip the ";" since we need to append the field
      // name
      if (!body.empty() && body.back() == ';')
        body.pop_back();

      std::string line;
      llvm::raw_string_ostream os(line);
      os << body;
      if (!F->getName().empty())
        os << " " << F->getNameAsString();
      os << array_suffix << ";";
      return line;
    }
    // for named nested record, don't do anything specific
  }

  FieldPromotion promo;
  if (ComputePromotion(F, Ctx, promo)) {
    std::string line;
    llvm::raw_string_ostream os(line);
    os << llvm::formatv("{0}{1} {2};", pad, promo.newTypeName, F->getNameAsString());
    if (promo.isBitField)
      os << llvm::formatv("  /* WARNING: promoted from bit-field (was {0} bits) */", promo.bitFieldWidth);
    if (promo.sizeDecreased)
      os << llvm::formatv("  /* WARNING: size decreased (original field was {0} bytes) */", promo.origBytes);
    return line;
  }

  // PrintingPolicy::printDecl() gives us   "type name[dims]"  correctly,
  // including multi-dimensional arrays, qualifiers, and typedef names
  clang::PrintingPolicy pp(Ctx.getLangOpts());
  pp.SuppressTagKeyword = false;
  pp.SuppressScope = false;
  pp.AnonymousTagLocations = false;

  std::string result = pad;
  llvm::raw_string_ostream os(result);
  // printDeclaration-style: pass the variable name so arrays print as T a[N].
  qt.print(os, pp, F->getNameAsString());
  os << ";";
  return os.str();
}

auto PrintRecordDecl(const clang::RecordDecl *RD, clang::ASTContext &Ctx, const std::string &overrideName,
                     unsigned indent) -> std::string {
  std::string const pad(indent, ' ');
  std::string const keyword = RD->isUnion() ? "union" : "struct";
  std::string const name = overrideName.empty() ? RD->getNameAsString() : overrideName;

  std::string out;
  llvm::raw_string_ostream os(out);
  os << pad << keyword;
  if (!name.empty())
    os << " " << name;
  os << " {\n";

  unsigned const field_indent = indent + 4;
  for (const clang::FieldDecl *f : RD->fields()) {
    os << PrintFieldDecl(f, Ctx, field_indent) << "\n";
  }

  os << pad << "}";
  return os.str();
}
} // namespace

auto PassFind::VisitRecordDecl(clang::RecordDecl *RD) -> bool {
  if (RD->isCompleteDefinition()) {
    bool inside_func = true;
    // only consider records inside functions
    for (const clang::DeclContext *dc = RD->getDeclContext(); dc != nullptr; dc = dc->getParent()) {

      if (const auto *fd = llvm::dyn_cast<clang::FunctionDecl>(dc)) {
        InitialisedStructs.insert({RD, fd});
        inside_func = true;
        break;
      }
    }

    if (!inside_func)
      InitialisedStructs.insert({RD, nullptr});
  }

  return true;
}

void Consumer::HandleTranslationUnit(clang::ASTContext &Ctx) {
  const auto &sm = Ctx.getSourceManager();
  PassFind sc;
  sc.TraverseDecl(Ctx.getTranslationUnitDecl());

  // copy source text to new file
  const llvm::MemoryBufferRef buf = sm.getBufferOrFake(sm.getMainFileID(), clang::SourceLocation{});
  std::string const source_text(buf.getBuffer());

  if (sc.InitialisedStructs.empty()) {
    return;
  }

  // Find offset just after the last top of file #include, and whether
  // <stdint.h> is already included anywhere in the file
  llvm::StringRef const main_file = sm.getFileEntryRefForID(sm.getMainFileID())->getName();
  unsigned insert_offset = 0;
  bool has_stdint = false;
  {
    llvm::StringRef const text(source_text);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t const line_end = text.find('\n', pos);
      llvm::StringRef const line =
          (line_end == llvm::StringRef::npos) ? text.substr(pos) : text.substr(pos, line_end - pos);
      llvm::StringRef const trimmed = line.trim();

      if (trimmed.contains("stdint.h"))
        has_stdint = true;

      if (trimmed.starts_with("#include")) {
        // Extend insertOffset to just past this line (including newline).
        insert_offset = (line_end == llvm::StringRef::npos) ? static_cast<unsigned>(source_text.size())
                                                            : static_cast<unsigned>(line_end + 1);
      } else if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("/*")) {
        // Skip blank lines / comments at the top without stopping the scan
      } else {
        // First non-include, non-blank, non-comment line: stop scanning
        break;
      }

      if (line_end == llvm::StringRef::npos)
        break;
      pos = line_end + 1;
    }
  }

  std::string preamble;
  {
    llvm::raw_string_ostream os(preamble);
    os << "/* c2pancake: promoted struct definitions */\n";
    if (!has_stdint) {
      os << "#include <stdint.h>\n\n";
    }
  }

  for (const auto &[RD, FD] : sc.InitialisedStructs) {
    if (RD == nullptr)
      continue;
    clang::SourceLocation const def_loc = RD->getBeginLoc();
    if (def_loc.isInvalid() || sm.isInSystemHeader(def_loc))
      continue;

    std::string orig_name = RD->getNameAsString();
    if (orig_name.empty())
      continue; // anonymous top-level struct

    bool const is_global = clang::isa<clang::TranslationUnitDecl>(RD->getDeclContext());
    bool const needs_lift = !is_global;

    std::string new_name =
        needs_lift ? RecordDeclPrefix(true, sc.hoist_record_decl_counter++, FD->getNameAsString(), orig_name)
                   : orig_name;

    std::string new_body;
    {
      llvm::raw_string_ostream os(new_body);
      os << PrintRecordDecl(RD, Ctx, new_name, 0) << ";";
    }

    if (needs_lift) {
      std::string const comment = llvm::formatv("/* struct {0} hoisted to global scope as {1} */", orig_name, new_name);
      AddReplacement(Ctx, RD->getSourceRange(), comment, true);

      {
        llvm::raw_string_ostream os(preamble);
        os << new_body << "\n\n";
      }

      RewriteTypeUses(RD, new_name, Ctx);
    } else {
      // rewrite in place
      AddReplacement(Ctx, RD->getSourceRange(), new_body, true);
    }
  }

  {
    llvm::raw_string_ostream os(preamble);
    os << "/* c2pancake: end of promoted struct definitions */\n\n";
  }

  InsertAtOffset(main_file, insert_offset, preamble);
}

void Consumer::AddReplacement(clang::ASTContext &Ctx, clang::SourceRange SR, const std::string &newText,
                              bool includeTerminatingSemicolon) {
  const auto &sm = Ctx.getSourceManager();
  const auto &lang_opts = Ctx.getLangOpts();

  if (includeTerminatingSemicolon) {
    auto end = SR.getEnd();
    auto next = clang::Lexer::findNextToken(end, sm, lang_opts);
    if (next && next->is(clang::tok::semi)) {
      SR.setEnd(next->getLocation());
    }
  }

  auto char_range = clang::CharSourceRange::getTokenRange(SR);
  clang::tooling::Replacement const repl(Ctx.getSourceManager(), char_range, newText);
  if (auto err = repls.add(repl)) {
    llvm::errs() << "Replacement conflict: " << llvm::toString(std::move(err)) << "\n";
  }
}

void Consumer::InsertAtOffset(llvm::StringRef file, unsigned offset, const std::string &text) {
  clang::tooling::Replacement const r(file, offset, 0, text);
  if (auto err = repls.add(r))
    llvm::errs() << "Insert conflict: " << llvm::toString(std::move(err)) << "\n";
}

namespace {
struct Renamer : clang::RecursiveASTVisitor<Renamer> {
  const clang::RecordDecl *Target;
  const std::string &NewName;
  Consumer &Parent;
  clang::ASTContext &Ctx;

  Renamer(const clang::RecordDecl *T, const std::string &N, Consumer &P, clang::ASTContext &C)
      : Target(T), NewName(N), Parent(P), Ctx(C) {}

  auto VisitTypeLoc(clang::TypeLoc TL) -> bool {
    const auto &sm = Ctx.getSourceManager();
    if (auto rtl = TL.getAs<clang::RecordTypeLoc>()) {
      const clang::RecordDecl *rd = rtl.getDecl();
      if (rd == nullptr)
        return true;
      // Match the definition or any forward declaration of the same record.
      if (rd->getDefinition() != Target && rd != Target)
        return true;

      clang::SourceRange const sr = rtl.getSourceRange();
      if (sr.isInvalid())
        return true;

      clang::SourceLocation const b = sm.getSpellingLoc(sr.getBegin());
      if (sm.isInSystemHeader(b))
        return true;

      std::string replacement;
      llvm::raw_string_ostream os(replacement);
      os << "struct " << NewName;
      Parent.AddReplacement(Ctx, sr, replacement, false);
    }

    return true;
  }
};
} // namespace

void Consumer::RewriteTypeUses(const clang::RecordDecl *RD, const std::string &newName, clang::ASTContext &Ctx) {
  Renamer r(RD, newName, *this, Ctx);
  r.TraverseDecl(Ctx.getTranslationUnitDecl());
}
