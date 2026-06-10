#include "ir_builder.h"
#include "pancake_ir.h"
#include "util.h"

#include <cassert>
#include <clang-c/Index.h>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ranges>
#include <regex>

namespace pancake {

IRBuilder::~IRBuilder() {
  if (tu)
    clang_disposeTranslationUnit(tu);
  if (index)
    clang_disposeIndex(index);
}

// helpers
std::string IRBuilder::resolveVarName(CXCursor cursor) {
  auto kind = clang_getCursorKind(cursor);
  assert(kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr);

  std::string name = getCursorSpelling(cursor);
  if (name.empty()) {
    auto children = getChildren(cursor);
    if (!children.empty())
      name = getCursorSpelling(children[0]);
  }
  return name;
}

std::string IRBuilder::arrayAddr(int baseSlot, const std::string &indexExpr) {
  return "(@base + (" + std::to_string(baseSlot) + " + " + indexExpr +
         ") * @biw)";
}

std::string IRBuilder::arrayAddr(int slot) {
  return "(@base + " + std::to_string(slot) + " * @biw)";
}

BlockPtr IRBuilder::buildBlockOrWrap(CXCursor cursor) {
  if (clang_getCursorKind(cursor) == CXCursor_CompoundStmt)
    return buildBlock(cursor);
  auto block = std::make_shared<Block>();
  buildStmt(cursor, block->stmts);
  return block;
}

std::optional<BinOp> IRBuilder::lookupBinOp(const std::string &opStr) {
  static const std::map<std::string, BinOp> ops = {
      {"+", BinOp::Add},       {"-", BinOp::Sub},
      {"*", BinOp::Mul},       {"/", BinOp::Div},
      {"%", BinOp::Mod},       {"<", BinOp::Lt},
      {">", BinOp::Gt},        {"<=", BinOp::Le},
      {">=", BinOp::Ge},       {"==", BinOp::Eq},
      {"!=", BinOp::Ne},       {"&&", BinOp::And},
      {"||", BinOp::Or},       {"&", BinOp::BitwiseAnd},
      {"|", BinOp::BitwiseOr}, {"^", BinOp::BitwiseXor},
      {"<<", BinOp::Shl},      {">>", BinOp::Shr},
  };
  auto it = ops.find(opStr);
  return (it != ops.end()) ? std::optional(it->second) : std::nullopt;
}

std::optional<UnaryOp> IRBuilder::lookupUnaryOp(const std::string &opStr) {
  static const std::map<std::string, UnaryOp> ops = {
      {"-", UnaryOp::Negate},
      {"!", UnaryOp::Not},
      {"~", UnaryOp::BitwiseNot},
  };
  auto it = ops.find(opStr);
  return (it != ops.end()) ? std::optional(it->second) : std::nullopt;
}

int IRBuilder::findStructFieldIndex(const std::string &varName,
                                    const std::string &fieldName) {
  auto svIt = structVars.find(varName);
  if (svIt == structVars.end())
    return -1;
  auto stIt = structTypes.find(svIt->second);
  if (stIt == structTypes.end())
    return -1;
  const auto &fields = stIt->second.fieldNames;
  for (int i = 0; i < (int)fields.size(); i++) {
    if (fields[i] == fieldName)
      return i;
  }
  return -1;
}

int IRBuilder::getStructFieldCount(const std::string &varName) {
  auto svIt = structVars.find(varName);
  if (svIt == structVars.end())
    return 0;
  auto stIt = structTypes.find(svIt->second);
  if (stIt == structTypes.end())
    return 0;
  return (int)stIt->second.fieldNames.size();
}

std::string IRBuilder::stripStructPrefix(const std::string &name) {
  if (name.size() > 7 && name.substr(0, 7) == "struct ")
    return name.substr(7);
  return name;
}

// FFI annotation scanner

void IRBuilder::scanFFIAnnotations(const std::string &filename) {
  std::ifstream file(filename);
  if (!file)
    return;

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line))
    lines.push_back(line);

  // "// @ffi: name" -> replace next stmt with @name(0,0,0,0)
  std::regex ffiStmtTag(R"(^\s*//\s*@ffi:\s*(\w+)\s*$)");

  for (size_t i = 0; i < lines.size(); i++) {
    std::smatch sm;
    if (std::regex_match(lines[i], sm, ffiStmtTag)) {
      std::string ffiName = sm[1].str();
      // find next non-blank, non-comment line
      for (size_t j = i + 1; j < lines.size(); j++) {
        std::string trimmed = lines[j];
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.empty() || trimmed.substr(0, 2) == "//")
          continue;
        // line numbers are 1-based in libclang
        ffiLineReplacements[j + 1] = ffiName;
        break;
      }
    }
  }
}

std::unique_ptr<Program> IRBuilder::build(const std::string &filename) {
  scanFFIAnnotations(filename);
  index = clang_createIndex(0, 0);

  const char *args[] = {"-std=c11", "-I/usr/include", "-I/usr/local/include"};
  tu =
      clang_parseTranslationUnit(index, filename.c_str(), args, 3, nullptr, 0,
                                 CXTranslationUnit_DetailedPreprocessingRecord);
  if (!tu) {
    std::cerr << "Failed to parse " << filename << std::endl;
    return nullptr;
  }

  auto program = std::make_unique<Program>();
  currentProgram = program.get();
  CXCursor root = clang_getTranslationUnitCursor(tu);

  // walk top-level declarations
  struct Ctx {
    IRBuilder *self;
    Program *prog;
  };
  Ctx ctx{this, program.get()};

  clang_visitChildren(
      root,
      [](CXCursor cursor, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto *ctx = static_cast<Ctx *>(data);
        if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)))
          return CXChildVisit_Continue;

        auto kind = clang_getCursorKind(cursor);
        if (kind == CXCursor_FunctionDecl) {
          auto func = ctx->self->buildFunction(cursor);
          if (func)
            ctx->prog->functions.push_back(func);
        } else if (kind == CXCursor_VarDecl) {
          ctx->self->buildVarDecl(cursor, ctx->prog->globals);
        } else if (kind == CXCursor_StructDecl) {
          ctx->self->buildStructDecl(cursor, ctx->prog->globals);
        } else if (kind == CXCursor_TypedefDecl) {
          ctx->prog->globals.push_back(std::make_shared<CommentStmt>(
              "TODO: Typedef: " + ctx->self->getSourceText(cursor),
              ctx->self->getLoc(cursor)));
        } else if (kind == CXCursor_EnumDecl) {
          ctx->self->buildEnumDecl(cursor, ctx->prog->globals);
        }
        return CXChildVisit_Continue;
      },
      &ctx);

  // inject global array inits into main's body
  if (!program->arrayInits.empty()) {
    for (auto &func : program->functions) {
      if (func->name == "main" && func->body) {
        auto &body = func->body->stmts;
        body.insert(body.begin(),
                    std::make_move_iterator(program->arrayInits.begin()),
                    std::make_move_iterator(program->arrayInits.end()));
        program->arrayInits.clear();
        break;
      }
    }
  }

  return program;
}

std::shared_ptr<Function> IRBuilder::buildFunction(CXCursor cursor) {
  FunctionScope scope(insideFunction);

  auto func = std::make_shared<Function>();
  func->name = getCursorSpelling(cursor);
  func->loc = getLoc(cursor);

  auto children = getChildren(cursor);
  for (auto &child : children) {
    auto kind = clang_getCursorKind(child);
    if (kind == CXCursor_ParmDecl) {
      Param p;
      p.name = getCursorSpelling(child);
      p.shape = 1;

      // check if param is a struct type
      CXType canonParamType =
          clang_getCanonicalType(clang_getCursorType(child));
      if (canonParamType.kind == CXType_Record) {
        CXString pts = clang_getTypeSpelling(canonParamType);
        std::string paramTypeName = stripStructPrefix(clang_getCString(pts));
        clang_disposeString(pts);
        auto stIt = structTypes.find(paramTypeName);
        if (stIt != structTypes.end()) {
          p.shape = (int)stIt->second.fieldNames.size();
          structVars[p.name] = paramTypeName;
        }
      }

      func->params.push_back(p);
    } else if (kind == CXCursor_CompoundStmt) {
      func->body = buildBlock(child);
    }
  }

  if (!func->body)
    func->body = std::make_shared<Block>();

  // Pancake requires all functions to end with return
  auto &stmts = func->body->stmts;
  if (stmts.empty() || stmts.back()->kind != StmtKind::Return)
    stmts.push_back(std::make_shared<ReturnStmt>(nullptr, func->loc));

  return func;
}

BlockPtr IRBuilder::buildBlock(CXCursor cursor) {
  auto block = std::make_shared<Block>();
  block->loc = getLoc(cursor);

  for (auto &child : getChildren(cursor))
    buildStmt(child, block->stmts);
  return block;
}

void IRBuilder::buildStmt(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)))
    return;

  auto kind = clang_getCursorKind(cursor);
  auto loc = getLoc(cursor);

  // check for FFI replacement
  auto ffiIt = ffiLineReplacements.find(loc.line);
  if (ffiIt != ffiLineReplacements.end()) {
    stmts.push_back(std::make_shared<ExprStmt>(
        std::make_shared<RawExpr>("@" + ffiIt->second + "(0,0,0,0)", loc),
        loc));
    return;
  }

  // TODO everything other than compound statement needs to handle hoisting
  // first!
  switch (kind) {
  case CXCursor_DeclStmt: {
    for (auto &child : getChildren(cursor))
      buildStmt(child, stmts);
    break;
  }
  case CXCursor_VarDecl:
    buildVarDecl(cursor, stmts);
    break;

  case CXCursor_CompoundStmt: {
    auto block = buildBlock(cursor);
    for (auto &s : block->stmts)
      stmts.push_back(s);
    break;
  }
  case CXCursor_ReturnStmt: {
    auto children = getChildren(cursor);
    ExprPtr value;
    if (!children.empty())
      value = buildExpr(children[0]);
    stmts.push_back(std::make_shared<ReturnStmt>(value, loc));
    break;
  }
  case CXCursor_IfStmt: {
    auto children = getChildren(cursor);
    if (children.size() < 2)
      break;

    // hoist array loads from condition
    ExprPtr cond = hoistArrayLoads(children[0], stmts);
    if (!cond)
      cond = buildExpr(children[0]);

    auto thenBranch = buildBlockOrWrap(children[1]);

    BlockPtr elseBranch;
    if (children.size() >= 3)
      elseBranch = buildBlockOrWrap(children[2]);

    stmts.push_back(
        std::make_shared<IfStmt>(cond, thenBranch, elseBranch, loc));
    break;
  }
  case CXCursor_WhileStmt: {
    auto children = getChildren(cursor);
    if (children.size() < 2)
      break;

    auto cond = buildExpr(children[0]);
    auto body = buildBlockOrWrap(children[1]);

    stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
    break;
  }
  case CXCursor_ForStmt:
    buildForStmt(cursor, stmts);
    break;

  case CXCursor_DoStmt: {
    // do-while -> body once, then while loop
    auto children = getChildren(cursor);
    if (children.size() < 2)
      break;

    CXCursor bodyCursor = children[0];
    CXCursor condCursor = children[1];

    // emit body once (the "do" part)
    if (clang_getCursorKind(bodyCursor) == CXCursor_CompoundStmt) {
      auto block = buildBlock(bodyCursor);
      for (auto &s : block->stmts)
        stmts.push_back(s);
    } else {
      buildStmt(bodyCursor, stmts);
    }

    // build while loop (re-parses body cursor)
    auto cond = buildExpr(condCursor);
    auto body = buildBlockOrWrap(bodyCursor);

    stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
    break;
  }

  case CXCursor_BreakStmt:
    stmts.push_back(std::make_shared<BreakStmt>(loc));
    break;

  case CXCursor_ContinueStmt:
    stmts.push_back(std::make_shared<ContinueStmt>(loc));
    break;

  case CXCursor_CompoundAssignOperator:
    buildCompoundAssign(cursor, stmts);
    break;

  case CXCursor_UnaryOperator: {
    auto result = tryBuildIncrDecr(cursor);
    if (result) {
      stmts.push_back(result);
    } else {
      pushExprStmt(stmts, cursor);
    }
    break;
  }
  case CXCursor_CallExpr: {
    std::string name = getCalleeName(cursor);
    if (name == "printf") {
      stmts.push_back(std::make_shared<CommentStmt>(
          "TODO: printf not available in Pancake - " + getSourceText(cursor),
          loc));
    } else {
      stmts.push_back(std::make_shared<ExprStmt>(buildExpr(cursor), loc));
    }
    break;
  }
  case CXCursor_BinaryOperator: {
    auto children = getChildren(cursor);
    std::string opStr = extractBinOp(cursor);
    if (opStr == "=" && children.size() == 2) {
      // struct field assignment
      if (clang_getCursorKind(children[0]) == CXCursor_MemberRefExpr) {
        auto memberChildren = getChildren(children[0]);
        std::string fieldName = getCursorSpelling(children[0]);
        if (!memberChildren.empty()) {
          std::string varName = resolveVarName(memberChildren[0]);
          int fi = findStructFieldIndex(varName, fieldName);
          if (fi >= 0) {
            int fc = getStructFieldCount(varName);
            auto value = buildExpr(children[1]);
            stmts.push_back(std::make_shared<StructFieldAssignStmt>(
                varName, fi, fc, value, loc));
            return;
          }
        }
      }
      // array element assignment
      if (clang_getCursorKind(children[0]) == CXCursor_ArraySubscriptExpr) {
        auto arrChildren = getChildren(children[0]);
        if (arrChildren.size() == 2) {
          std::string arrName = resolveVarName(arrChildren[0]);
          auto ait = globalArrays.find(arrName);
          if (ait != globalArrays.end()) {
            std::string addr =
                arrayAddr(ait->second.baseSlot, getSourceText(arrChildren[1]));
            // hoist RHS if it contains array access
            ExprPtr srcVar = hoistArrayLoads(children[1], stmts);
            if (!srcVar) {
              srcVar = buildExpr(children[1]);
              // emit store via temp var
              std::string valTmp = std::format("av{}", arrayTmpCounter++);
              stmts.push_back(std::make_shared<VarDeclStmt>(valTmp, srcVar,
                                                            std::nullopt, loc));
              srcVar = std::make_shared<VarRefExpr>(valTmp, loc);
            }

            // TODO we need to parse the LHS for any array access and so on as
            // well
            auto destVar = std::make_shared<RawExpr>(addr, loc);
            stmts.push_back(
                std::make_shared<MemoryStoreStmt>(srcVar, destVar, loc));
            return;
          }
        }
      }
      std::string target = getCursorSpelling(children[0]);
      // hoist array loads from RHS
      auto hoisted = hoistArrayLoads(children[1], stmts);
      if (hoisted) {
        stmts.push_back(std::make_shared<AssignStmt>(target, hoisted, loc));
        return;
      }
      // expand ternary in RHS
      auto ternaryResult = tryExpandTernary(children[1], stmts);
      if (ternaryResult) {
        stmts.push_back(
            std::make_shared<AssignStmt>(target, ternaryResult, loc));
      } else {
        auto value = buildExpr(children[1]);
        stmts.push_back(std::make_shared<AssignStmt>(target, value, loc));
      }
    } else {
      pushExprStmt(stmts, cursor);
    }
    break;
  }
  case CXCursor_SwitchStmt:
    buildSwitchStmt(cursor, stmts);
    break;

  default:
    pushExprStmt(stmts, cursor);
    break;
  }
}

void IRBuilder::buildVarDecl(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  std::string name = getCursorSpelling(cursor);
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);

  // array declaration
  CXType varType = clang_getCursorType(cursor);
  if (varType.kind == CXType_ConstantArray) {
    long long arrSize = clang_getArraySize(varType);
    if (arrSize > 0) {
      ArrayInfo info{nextArraySlot, (int)arrSize};
      globalArrays[name] = info;
      nextArraySlot += (int)arrSize;

      stmts.push_back(std::make_shared<CommentStmt>(
          "array " + name + "[" + std::to_string(arrSize) + "] → @base slots " +
              std::to_string(info.baseSlot) + ".." +
              std::to_string(info.baseSlot + info.size - 1),
          loc));

      auto &target = insideFunction ? stmts : currentProgram->arrayInits;
      for (auto &child : children) {
        if (clang_getCursorKind(child) == CXCursor_InitListExpr) {
          auto initChildren = getChildren(child);
          for (int i = 0; i < (int)initChildren.size() && i < (int)arrSize;
               i++) {
            target.push_back(std::make_shared<ExprStmt>(
                std::make_shared<RawExpr>("st " + arrayAddr(info.baseSlot + i) +
                                              ", " +
                                              getSourceText(initChildren[i]),
                                          loc),
                loc));
          }
        }
      }
      return;
    }
  }

  // struct variable
  CXType canonType = clang_getCanonicalType(varType);
  if (canonType.kind == CXType_Record) {
    CXString typeSpelling = clang_getTypeSpelling(canonType);
    std::string structName = stripStructPrefix(clang_getCString(typeSpelling));
    clang_disposeString(typeSpelling);

    auto it = structTypes.find(structName);
    if (it != structTypes.end()) {
      const auto &info = it->second;
      int fieldCount = (int)info.fieldNames.size();

      structVars[name] = structName;

      ExprPtr init;
      if (!children.empty()) {
        auto lastChild = children.back();
        auto lastKind = clang_getCursorKind(lastChild);

        if (lastKind == CXCursor_InitListExpr) {
          auto initChildren = getChildren(lastChild);
          std::vector<ExprPtr> fieldExprs;
          for (auto &ic : initChildren)
            fieldExprs.push_back(buildExpr(ic));
          init = std::make_shared<StructLitExpr>(fieldExprs, loc);
        } else {
          init = buildExpr(lastChild);
        }
      }

      stmts.push_back(
          std::make_shared<VarDeclStmt>(name, init, fieldCount, loc));
      return;
    }
  }

  ExprPtr init;
  std::optional<int> shape;

  if (!children.empty()) {
    // hoist array loads from initializer
    auto hoisted = hoistArrayLoads(children.back(), stmts);
    if (hoisted) {
      stmts.push_back(
          std::make_shared<VarDeclStmt>(name, hoisted, std::nullopt, loc));
      return;
    }

    // expand ternary initializer
    if (getDescendant(cursor, CXCursor_ConditionalOperator)) {
      auto ternaryResult = tryExpandTernary(children.back(), stmts);
      if (ternaryResult) {
        stmts.push_back(std::make_shared<VarDeclStmt>(name, ternaryResult,
                                                      std::nullopt, loc));
        return;
      }
    }

    // function call in initializer needs shape annotation
    if (getDescendant(cursor, CXCursor_CallExpr))
      shape = 1;

    init = buildExpr(children.back());

    // emit unsupported initializers as comments to avoid invalid Pancake
    if (init && init->kind == ExprKind::Raw) {
      auto &raw = static_cast<RawExpr &>(*init);
      if (raw.text.find("TODO") != std::string::npos) {
        stmts.push_back(std::make_shared<CommentStmt>(
            "TODO: " + getSourceText(cursor), loc));
        return;
      }
    }
  }

  stmts.push_back(std::make_shared<VarDeclStmt>(name, init, shape, loc));
}

// for -> while conversion
IRBuilder::ForParts IRBuilder::classifyForChildren(CXCursor forStmt) {
  ForParts parts{};
  auto children = getChildren(forStmt);
  if (children.empty())
    return parts;

  // body is always the last child
  parts.body = children.back();

  if (children.size() == 1)
    return parts; // for(;;) body

  // find semicolons in for-header via source text
  unsigned forStartOffset;
  CXFile file;
  clang_getSpellingLocation(clang_getRangeStart(clang_getCursorExtent(forStmt)),
                            &file, nullptr, nullptr, &forStartOffset);

  const std::string &src = getFileContent(file);
  if (src.empty())
    return parts;

  size_t openParen = src.find('(', forStartOffset);
  if (openParen == std::string::npos)
    return parts;

  // find semicolons respecting nesting
  int depth = 0;
  size_t semi1 = std::string::npos, semi2 = std::string::npos;
  for (size_t i = openParen + 1; i < src.size(); i++) {
    char c = src[i];
    if (c == '(')
      depth++;
    else if (c == ')') {
      if (depth == 0)
        break;
      depth--;
    } else if (c == ';' && depth == 0) {
      if (semi1 == std::string::npos)
        semi1 = i;
      else {
        semi2 = i;
        break;
      }
    }
  }
  if (semi1 == std::string::npos || semi2 == std::string::npos)
    return parts;

  // classify children by position relative to semicolons
  for (size_t i = 0; i + 1 < children.size(); i++) {
    unsigned childOffset;
    clang_getSpellingLocation(
        clang_getRangeStart(clang_getCursorExtent(children[i])), nullptr,
        nullptr, nullptr, &childOffset);

    if (childOffset < semi1) {
      parts.init = children[i];
      parts.hasInit = true;
    } else if (childOffset < semi2) {
      parts.condition = children[i];
      parts.hasCond = true;
    } else {
      parts.update = children[i];
      parts.hasUpdate = true;
    }
  }
  return parts;
}

void IRBuilder::buildForStmt(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  auto loc = getLoc(cursor);
  auto parts = classifyForChildren(cursor);

  if (parts.hasInit)
    buildStmt(parts.init, stmts);

  ExprPtr cond = parts.hasCond ? buildExpr(parts.condition)
                               : std::make_shared<IntLitExpr>(1, loc);

  auto body = buildBlockOrWrap(parts.body);

  if (parts.hasUpdate)
    buildForUpdate(parts.update, body->stmts);

  stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
}

void IRBuilder::buildForUpdate(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  auto kind = clang_getCursorKind(cursor);

  if (kind == CXCursor_UnaryOperator) {
    auto result = tryBuildIncrDecr(cursor);
    if (result) {
      stmts.push_back(result);
      return;
    }
  }
  if (kind == CXCursor_CompoundAssignOperator) {
    buildCompoundAssign(cursor, stmts);
    return;
  }
  if (kind == CXCursor_BinaryOperator) {
    auto children = getChildren(cursor);
    std::string opStr = extractBinOp(cursor);
    if (opStr == "=" && children.size() == 2) {
      std::string target = getCursorSpelling(children[0]);
      auto value = buildExpr(children[1]);
      stmts.push_back(
          std::make_shared<AssignStmt>(target, value, getLoc(cursor)));
      return;
    }
  }
  pushExprStmt(stmts, cursor);
}

void IRBuilder::pushExprStmt(std::vector<StmtPtr> &stmts, CXCursor cursor) {
  if (cursorHasSideEffects(cursor))
    stmts.push_back(
        std::make_shared<ExprStmt>(buildExpr(cursor), getLoc(cursor)));
  else
    stmts.push_back(std::make_shared<CommentStmt>(
        "No side effects: " + getSourceText(cursor), getLoc(cursor)));
}

bool IRBuilder::cursorHasSideEffects(CXCursor cursor) {
  auto k = clang_getCursorKind(cursor);

  if (k == CXCursor_CallExpr || k == CXCursor_CompoundAssignOperator)
    return true;

  if (k == CXCursor_BinaryOperator) {
    auto op = extractBinOp(cursor);
    if (op == "=")
      return true;
  }

  if (k == CXCursor_UnaryOperator) {
    auto info = extractUnaryOp(cursor);
    if (info.op == "++" || info.op == "--")
      return true;
  }

  // treat reads of volatile variables as side-effects
  if (k == CXCursor_DeclRefExpr) {
    auto ref = clang_getCursorReferenced(cursor);
    if (clang_getCursorKind(ref) == CXCursor_VarDecl ||
        clang_getCursorKind(ref) == CXCursor_FieldDecl) {
      auto t = clang_getCursorType(ref);
      if (clang_isVolatileQualifiedType(t))
        return true;

      // this check is more expensive so it's gated behind the lighter one
      auto ct = clang_getCanonicalType(t);
      if (clang_isVolatileQualifiedType(ct))
        return true;
    }
  }

  for (auto &c : getChildren(cursor)) {
    if (cursorHasSideEffects(c))
      return true;
  }

  return false;
}

// compound assignment expansion (a += b -> a = a + b)
void IRBuilder::buildCompoundAssign(CXCursor cursor,
                                    std::vector<StmtPtr> &stmts) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);

  if (children.size() != 2) {
    stmts.push_back(std::make_shared<CommentStmt>(
        "TODO: unsupported compound assignment: " + getSourceText(cursor),
        loc));
    return;
  }

  // array access on LHS: arr[i] += v
  if (clang_getCursorKind(children[0]) == CXCursor_ArraySubscriptExpr) {
    auto arrChildren = getChildren(children[0]);
    if (arrChildren.size() == 2) {
      std::string arrName = resolveVarName(arrChildren[0]);
      auto ait = globalArrays.find(arrName);
      if (ait != globalArrays.end()) {
        std::string idxText = getSourceText(arrChildren[1]);
        std::string addr = arrayAddr(ait->second.baseSlot, idxText);
        std::string opStr2 = extractBinOp(cursor);
        std::string baseOpStr = opStr2.substr(0, opStr2.size() - 1);

        // hoist: load, compute, store
        std::string ldTmp = "al" + std::to_string(arrayTmpCounter++);
        stmts.push_back(std::make_shared<VarDeclStmt>(
            ldTmp, std::make_shared<RawExpr>("lds 1 " + addr, loc),
            std::nullopt, loc));

        auto rhs = buildExpr(children[1]);
        std::string resTmp = "al" + std::to_string(arrayTmpCounter++);
        auto baseOp = lookupBinOp(baseOpStr);
        if (baseOp) {
          auto compExpr = std::make_shared<BinaryExpr>(
              *baseOp, std::make_shared<VarRefExpr>(ldTmp, loc), rhs, loc);
          stmts.push_back(std::make_shared<VarDeclStmt>(resTmp, compExpr,
                                                        std::nullopt, loc));
        } else {
          stmts.push_back(std::make_shared<VarDeclStmt>(
              resTmp,
              std::make_shared<RawExpr>(ldTmp + " " + baseOpStr + " " +
                                            getSourceText(children[1]),
                                        loc),
              std::nullopt, loc));
        }
        stmts.push_back(std::make_shared<ExprStmt>(
            std::make_shared<RawExpr>("st " + addr + ", " + resTmp, loc), loc));
        return;
      }
    }
    stmts.push_back(std::make_shared<CommentStmt>(
        "TODO: Array compound assignment - " + getSourceText(cursor), loc));
    return;
  }

  std::string opStr = extractBinOp(cursor);

  // extract base operator from compound form ("+=" -> "+")
  std::string baseOpStr = opStr.substr(0, opStr.size() - 1);
  auto baseOp = lookupBinOp(baseOpStr);
  if (!baseOp) {
    stmts.push_back(std::make_shared<CommentStmt>(
        "TODO: unsupported compound assignment: " + getSourceText(cursor),
        loc));
    return;
  }

  // division and modulo not supported in Pancake
  if (*baseOp == BinOp::Div || *baseOp == BinOp::Mod) {
    stmts.push_back(std::make_shared<CommentStmt>(
        "TODO: " + baseOpStr + " not available in Pancake at the moment - " +
            getSourceText(cursor),
        loc));
    return;
  }

  std::string target = getCursorSpelling(children[0]);
  ExprPtr rhs = buildExpr(children[1]);
  auto varRef = std::make_shared<VarRefExpr>(target, loc);
  auto expanded = std::make_shared<BinaryExpr>(*baseOp, varRef, rhs, loc);

  stmts.push_back(std::make_shared<AssignStmt>(target, expanded, loc));
}

void IRBuilder::buildStructDecl(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  std::string name = getCursorSpelling(cursor);

  StructInfo info;
  for (auto &child : getChildren(cursor)) {
    if (clang_getCursorKind(child) == CXCursor_FieldDecl)
      info.fieldNames.push_back(getCursorSpelling(child));
  }

  if (!name.empty() && !info.fieldNames.empty())
    structTypes[name] = info;
}

void IRBuilder::buildEnumDecl(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  auto loc = getLoc(cursor);
  for (auto &child : getChildren(cursor)) {
    if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl) {
      std::string name = getCursorSpelling(child);
      int64_t value = clang_getEnumConstantDeclValue(child);
      stmts.push_back(std::make_shared<DefineStmt>(name, value, loc));
    }
  }
}

// switch -> nested if/else

void IRBuilder::buildSwitchStmt(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);

  if (children.size() < 2) {
    stmts.push_back(std::make_shared<CommentStmt>(
        "TODO: unsupported switch: " + getSourceText(cursor), loc));
    return;
  }

  auto switchExpr = buildExpr(children[0]);
  std::string switchVar = "sw" + std::to_string(switchVarCounter++);

  stmts.push_back(
      std::make_shared<VarDeclStmt>(switchVar, switchExpr, std::nullopt, loc));

  // collect case groups
  struct CaseGroup {
    std::vector<ExprPtr> values; // empty means default
    std::vector<StmtPtr> body;
    SourceLoc loc;
  };
  std::vector<CaseGroup> groups;

  auto bodyChildren = getChildren(children[1]);

  // recursively unwrap nested case/default labels
  struct LabelInfo {
    std::vector<ExprPtr> values;
    CXCursor bodyCursor;
    bool hasBody = false;
  };

  std::function<LabelInfo(CXCursor)> unwrapLabels =
      [&](CXCursor c) -> LabelInfo {
    LabelInfo info;
    auto k = clang_getCursorKind(c);
    auto ch = getChildren(c);

    if (k == CXCursor_CaseStmt) {
      if (ch.size() >= 1)
        info.values.push_back(buildExpr(ch[0]));
      if (ch.size() >= 2) {
        auto subKind = clang_getCursorKind(ch[1]);
        if (subKind == CXCursor_CaseStmt || subKind == CXCursor_DefaultStmt) {
          auto sub = unwrapLabels(ch[1]);
          for (auto &v : sub.values)
            info.values.push_back(v);
          info.bodyCursor = sub.bodyCursor;
          info.hasBody = sub.hasBody;
        } else {
          info.bodyCursor = ch[1];
          info.hasBody = true;
        }
      }
    } else if (k == CXCursor_DefaultStmt) {
      if (!ch.empty()) {
        auto subKind = clang_getCursorKind(ch[0]);
        if (subKind == CXCursor_CaseStmt || subKind == CXCursor_DefaultStmt) {
          auto sub = unwrapLabels(ch[0]);
          for (auto &v : sub.values)
            info.values.push_back(v);
          info.bodyCursor = sub.bodyCursor;
          info.hasBody = sub.hasBody;
        } else {
          info.bodyCursor = ch[0];
          info.hasBody = true;
        }
      }
    }
    return info;
  };

  for (auto &child : bodyChildren) {
    auto k = clang_getCursorKind(child);

    if (k == CXCursor_CaseStmt || k == CXCursor_DefaultStmt) {
      auto info = unwrapLabels(child);

      CaseGroup group;
      group.values = info.values;
      group.loc = getLoc(child);

      if (info.hasBody) {
        auto innerKind = clang_getCursorKind(info.bodyCursor);
        if (innerKind == CXCursor_CompoundStmt) {
          auto block = buildBlock(info.bodyCursor);
          for (auto &s : block->stmts)
            group.body.push_back(s);
        } else if (innerKind == CXCursor_BreakStmt) {
          // skip
        } else {
          buildStmt(info.bodyCursor, group.body);
        }
      }

      groups.push_back(group);
    } else if (k == CXCursor_BreakStmt) {
      // skip
    } else {
      if (!groups.empty())
        buildStmt(child, groups.back().body);
    }
  }

  // remove trailing breaks from each group
  for (auto &g : groups) {
    while (!g.body.empty() && g.body.back()->kind == StmtKind::Break)
      g.body.pop_back();
  }

  // find default group
  int defaultIdx = -1;
  for (int i = 0; i < (int)groups.size(); i++) {
    if (groups[i].values.empty()) {
      defaultIdx = i;
      break;
    }
  }

  BlockPtr elseBranch;
  if (defaultIdx >= 0) {
    elseBranch = std::make_shared<Block>();
    elseBranch->stmts = groups[defaultIdx].body;
  }

  // build if/else chain back to front
  StmtPtr result;
  for (int i = (int)groups.size() - 1; i >= 0; i--) {
    if (i == defaultIdx)
      continue;

    auto &g = groups[i];

    // build condition: switchVar == val1 || switchVar == val2 || ...
    ExprPtr cond;
    for (auto &val : g.values) {
      auto eq = std::make_shared<BinaryExpr>(
          BinOp::Eq, std::make_shared<VarRefExpr>(switchVar, g.loc), val,
          g.loc);
      if (!cond) {
        cond = eq;
      } else {
        cond = std::make_shared<BinaryExpr>(BinOp::Or, cond, eq, g.loc);
      }
    }

    if (!cond)
      continue;

    auto thenBlock = std::make_shared<Block>();
    thenBlock->stmts = g.body;

    auto ifStmt = std::make_shared<IfStmt>(cond, thenBlock, elseBranch, g.loc);

    elseBranch = std::make_shared<Block>();
    elseBranch->stmts.push_back(ifStmt);
  }

  if (elseBranch && !elseBranch->stmts.empty()) {
    for (auto &s : elseBranch->stmts)
      stmts.push_back(s);
  }
}

ExprPtr IRBuilder::hoistArrayLoads(CXCursor top_cursor,
                                   std::vector<StmtPtr> &stmts) {
  // hoist array loads and deref ops as they require temp vars
  // we use a post-traversal to hoist innermost ops first

  // this function is called before

  struct StackFrame {
    CXCursor cursor;
    bool children_visited = false;
  };
  std::vector<StackFrame> stack(1);
  stack.emplace_back(top_cursor, false);

  std::unordered_map<CursorHash, ExprPtr> hoistedExprs;
  ExprPtr retval;

  while (!stack.empty()) {
    StackFrame &frame = stack.back();
    auto cursor = frame.cursor;

    if (!frame.children_visited) {
      frame.children_visited = true;

      auto children = getChildren(cursor);
      auto reverse_view = children | std::views::filter([](CXCursor c) {
                            auto kind = clang_getCursorKind(c);
                            return kind == CXCursor_ArraySubscriptExpr ||
                                   (kind == CXCursor_UnaryOperator &&
                                    clang_getCursorUnaryOperatorKind(c) ==
                                        CXUnaryOperator_Deref) ||
                                   kind == CXCursor_UnexposedExpr ||
                                   kind == CXCursor_ParenExpr ||
                                   kind == CXCursor_CStyleCastExpr;
                          }) |
                          std::views::reverse |
                          std::views::transform(
                              [](CXCursor c) { return StackFrame{c, false}; });
      stack.reserve(stack.size() + std::ranges::distance(reverse_view));
      stack.insert(stack.end(), reverse_view.begin(), reverse_view.end());
    } else {
      auto kind = clang_getCursorKind(cursor);
      auto loc = getLoc(cursor);
      auto children = getChildren(cursor);

      switch (kind) {
      case CXCursor_ArraySubscriptExpr: {
        if (children.size() == 2) {
          auto arrName = resolveVarName(children[0]);
          auto ait = globalArrays.find(arrName);
          // TODO need to support the cursed syntax of 42[arr] as well
          if (ait != globalArrays.end()) {
            auto tmpVar = std::format("al{}", arrayTmpCounter++);
            ExprPtr idxExpr = nullptr;

            auto range = clang_getCursorExtent(children[1]);
            CursorHash hash{clang_getRangeStart(range),
                            clang_getRangeEnd(range)};
            auto hoistedIt = hoistedExprs.find(hash);
            if (hoistedIt != hoistedExprs.end()) {
              // reuse previously hoisted tmpvar directly
              auto varRef = hoistedIt->second;

              // (@base + (baseSlot + idx) * @biw)
              // in this case idx is varRef
              idxExpr = std::make_shared<BinaryExpr>(
                  BinOp::Add, std::make_shared<RawExpr>("@base", loc),
                  std::make_shared<BinaryExpr>(
                      BinOp::Mul,
                      std::make_shared<BinaryExpr>(
                          BinOp::Add,
                          std::make_shared<RawExpr>(
                              std::format("{}", ait->second.baseSlot), loc),
                          varRef, loc),
                      std::make_shared<RawExpr>("@biw", loc), loc),
                  loc);
              hoistedExprs.erase(hoistedIt);
            } else {
              // create new temp var for this array access
              idxExpr = buildExpr(children[1]);
              auto parentRange = clang_getCursorExtent(cursor);
              CursorHash parentHash{clang_getRangeStart(parentRange),
                                    clang_getRangeEnd(parentRange)};
              hoistedExprs[parentHash] =
                  std::make_shared<VarRefExpr>(tmpVar, loc);
            }

            stmts.push_back(std::make_shared<VarDeclStmt>(tmpVar, idxExpr,
                                                          std::nullopt, loc));
            retval = std::make_shared<VarRefExpr>(tmpVar, loc);
          }
        }
        break;
      }
      case CXCursor_UnaryOperator: {
        // has to be deref
        if (children.size() == 1) {
          auto tmpVar = std::format("deref{}", derefTmpCounter++);
          ExprPtr operandExpr = nullptr;

          auto range = clang_getCursorExtent(children[0]);
          CursorHash hash{clang_getRangeStart(range), clang_getRangeEnd(range)};
          auto hoistedIt = hoistedExprs.find(hash);
          if (hoistedIt != hoistedExprs.end()) {
            // reuse previously hoisted deref tmpvar directly
            operandExpr = hoistedIt->second;
            hoistedExprs.erase(hoistedIt);
          } else {
            // create new temp var for this deref access
            operandExpr = buildExpr(children[0]);
          }

          stmts.push_back(std::make_shared<VarDeclStmt>(tmpVar, operandExpr,
                                                        std::nullopt, loc));
          hoistedExprs[hash] = std::make_shared<VarRefExpr>(tmpVar, loc);
          retval = std::make_shared<VarRefExpr>(tmpVar, loc);
        }
        break;
      }

      default:
        break;
      }

      stack.pop_back();
    }
  }

  return retval;
}

// ternary expansion (cond ? a : b -> tmp var + if/else)
ExprPtr IRBuilder::tryExpandTernary(CXCursor cursor,
                                    std::vector<StmtPtr> &stmts) {
  auto kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_UnexposedExpr || kind == CXCursor_ParenExpr) {
    auto children = getChildren(cursor);
    if (!children.empty())
      return tryExpandTernary(children[0], stmts);
    return nullptr;
  }

  if (kind != CXCursor_ConditionalOperator)
    return nullptr;

  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  if (children.size() < 3)
    return nullptr;

  std::string tmpVar = "tn" + std::to_string(ternaryVarCounter++);

  stmts.push_back(std::make_shared<VarDeclStmt>(
      tmpVar, std::make_shared<IntLitExpr>(0, loc), std::nullopt, loc));

  auto cond = buildExpr(children[0]);

  // then branch (may contain nested ternary)
  auto thenBlock = std::make_shared<Block>();
  auto nestedTrue = tryExpandTernary(children[1], thenBlock->stmts);
  if (nestedTrue) {
    thenBlock->stmts.push_back(
        std::make_shared<AssignStmt>(tmpVar, nestedTrue, loc));
  } else {
    thenBlock->stmts.push_back(
        std::make_shared<AssignStmt>(tmpVar, buildExpr(children[1]), loc));
  }

  // else branch (may contain nested ternary)
  auto elseBlock = std::make_shared<Block>();
  auto nestedFalse = tryExpandTernary(children[2], elseBlock->stmts);
  if (nestedFalse) {
    elseBlock->stmts.push_back(
        std::make_shared<AssignStmt>(tmpVar, nestedFalse, loc));
  } else {
    elseBlock->stmts.push_back(
        std::make_shared<AssignStmt>(tmpVar, buildExpr(children[2]), loc));
  }

  stmts.push_back(std::make_shared<IfStmt>(cond, thenBlock, elseBlock, loc));

  return std::make_shared<VarRefExpr>(tmpVar, loc);
}

ExprPtr IRBuilder::buildExpr(CXCursor cursor) {
  auto kind = clang_getCursorKind(cursor);
  auto loc = getLoc(cursor);

  switch (kind) {
  case CXCursor_IntegerLiteral:
    return buildIntLit(cursor);

  case CXCursor_FloatingLiteral:
  case CXCursor_StringLiteral:
  case CXCursor_CharacterLiteral:
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  case CXCursor_DeclRefExpr:
    return std::make_shared<VarRefExpr>(getCursorSpelling(cursor), loc);

  case CXCursor_BinaryOperator:
    return buildBinaryExpr(cursor);

  case CXCursor_UnaryOperator:
    return buildUnaryExpr(cursor);

  case CXCursor_CallExpr:
    return buildCallExpr(cursor);

  case CXCursor_ParenExpr:
  case CXCursor_UnexposedExpr: {
    auto children = getChildren(cursor);
    if (!children.empty())
      return buildExpr(children[0]);
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);
  }
  case CXCursor_CStyleCastExpr: {
    auto children = getChildren(cursor);
    if (!children.empty())
      return buildExpr(children.back());
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);
  }
  // this only handles the TRIVIAL case!
  // For nested index access and so on, see hoistArrayLoads
  case CXCursor_ArraySubscriptExpr: {
    auto arrChildren = getChildren(cursor);
    if (arrChildren.size() == 2) {
      std::string arrName = resolveVarName(arrChildren[0]);
      auto ait = globalArrays.find(arrName);
      if (ait != globalArrays.end()) {
        std::string addr =
            arrayAddr(ait->second.baseSlot, getSourceText(arrChildren[1]));
        return std::make_shared<RawExpr>("lds 1 " + addr, loc);
      }
    }
    return std::make_shared<RawExpr>(
        "/* TODO: Array access - " + getSourceText(cursor) + " */", loc);
  }

  case CXCursor_MemberRefExpr: {
    auto memberChildren = getChildren(cursor);
    std::string fieldName = getCursorSpelling(cursor);
    if (!memberChildren.empty()) {
      std::string varName = resolveVarName(memberChildren[0]);
      int fi = findStructFieldIndex(varName, fieldName);
      if (fi >= 0)
        return std::make_shared<FieldAccessExpr>(varName, fi, loc);
    }
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);
  }

  case CXCursor_ConditionalOperator:
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  case CXCursor_InitListExpr: {
    auto children = getChildren(cursor);
    std::string result = "<";
    for (size_t i = 0; i < children.size(); i++) {
      if (i > 0)
        result += ", ";
      result += getSourceText(children[i]);
    }
    result += ">";
    return std::make_shared<RawExpr>(result, loc);
  }
  case CXCursor_CompoundAssignOperator:
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  default:
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);
  }
}

ExprPtr IRBuilder::buildBinaryExpr(CXCursor cursor) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);

  if (children.size() != 2)
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  std::string opStr = extractBinOp(cursor);

  if (opStr == "=")
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  auto op = lookupBinOp(opStr);
  if (!op)
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  if (*op == BinOp::Div || *op == BinOp::Mod) {
    std::string opName = (*op == BinOp::Div) ? "Division" : "Modulo";
    return std::make_shared<RawExpr>(
        "/* TODO: " + opName + " not available in Pancake at the moment - " +
            getSourceText(cursor) + " */",
        loc);
  }

  auto lhs = buildExpr(children[0]);
  auto rhs = buildExpr(children[1]);
  return std::make_shared<BinaryExpr>(*op, lhs, rhs, loc);
}

ExprPtr IRBuilder::buildUnaryExpr(CXCursor cursor) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  if (children.empty())
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  auto info = extractUnaryOp(cursor);

  if (info.op == "++" || info.op == "--")
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  auto op = lookupUnaryOp(info.op);
  if (!op)
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  auto operand = buildExpr(children[0]);
  return std::make_shared<UnaryExpr>(*op, operand, loc);
}

ExprPtr IRBuilder::buildCallExpr(CXCursor cursor) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  if (children.empty())
    return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  std::string callee = getCalleeName(cursor);

  if (callee == "printf") {
    return std::make_shared<RawExpr>(
        "/* TODO: printf not available in Pancake - " + getSourceText(cursor) +
            " */",
        loc);
  }

  std::vector<ExprPtr> args;
  for (size_t i = 1; i < children.size(); i++)
    args.push_back(buildExpr(children[i]));

  return std::make_shared<CallExpr>(callee, args, loc);
}

ExprPtr IRBuilder::buildIntLit(CXCursor cursor) {
  std::string text = getSourceText(cursor);
  try {
    int64_t value = std::stoll(text, nullptr, 0);
    return std::make_shared<IntLitExpr>(value, getLoc(cursor));
  } catch (...) {
    return std::make_shared<RawExpr>(text, getLoc(cursor));
  }
}

// increment/decrement (i++ -> i = i + 1)
StmtPtr IRBuilder::tryBuildIncrDecr(CXCursor cursor) {
  auto info = extractUnaryOp(cursor);
  if (info.op != "++" && info.op != "--")
    return nullptr;

  auto children = getChildren(cursor);
  if (children.empty())
    return nullptr;

  std::string varName = getCursorSpelling(children[0]);
  if (varName.empty())
    return nullptr;

  auto loc = getLoc(cursor);
  BinOp op = (info.op == "++") ? BinOp::Add : BinOp::Sub;
  auto varRef = std::make_shared<VarRefExpr>(varName, loc);
  auto one = std::make_shared<IntLitExpr>(1, loc);
  auto expr = std::make_shared<BinaryExpr>(op, varRef, one, loc);

  return std::make_shared<AssignStmt>(varName, expr, loc);
}

// operator extraction via tokenization
std::string IRBuilder::extractBinOp(CXCursor cursor) {
  auto children = getChildren(cursor);
  if (children.size() < 2)
    return "";

  CXSourceRange range = clang_getCursorExtent(cursor);
  CXToken *tokens = nullptr;
  unsigned numTokens = 0;
  clang_tokenize(tu, range, &tokens, &numTokens);

  unsigned lhsEndOffset, rhsStartOffset;
  clang_getSpellingLocation(
      clang_getRangeEnd(clang_getCursorExtent(children[0])), nullptr, nullptr,
      nullptr, &lhsEndOffset);
  clang_getSpellingLocation(
      clang_getRangeStart(clang_getCursorExtent(children[1])), nullptr, nullptr,
      nullptr, &rhsStartOffset);

  std::string result;
  for (unsigned i = 0; i < numTokens; i++) {
    unsigned tokOffset;
    clang_getSpellingLocation(clang_getTokenLocation(tu, tokens[i]), nullptr,
                              nullptr, nullptr, &tokOffset);

    if (tokOffset >= lhsEndOffset && tokOffset < rhsStartOffset) {
      CXString spelling = clang_getTokenSpelling(tu, tokens[i]);
      result = clang_getCString(spelling);
      clang_disposeString(spelling);
      break;
    }
  }

  clang_disposeTokens(tu, tokens, numTokens);
  return result;
}

IRBuilder::UnaryOpInfo IRBuilder::extractUnaryOp(CXCursor cursor) {
  auto children = getChildren(cursor);
  if (children.empty())
    return {"", false};

  CXSourceRange range = clang_getCursorExtent(cursor);
  CXToken *tokens = nullptr;
  unsigned numTokens = 0;
  clang_tokenize(tu, range, &tokens, &numTokens);

  unsigned childStartOffset, childEndOffset;
  clang_getSpellingLocation(
      clang_getRangeStart(clang_getCursorExtent(children[0])), nullptr, nullptr,
      nullptr, &childStartOffset);
  clang_getSpellingLocation(
      clang_getRangeEnd(clang_getCursorExtent(children[0])), nullptr, nullptr,
      nullptr, &childEndOffset);

  UnaryOpInfo info{"", false};
  for (unsigned i = 0; i < numTokens; i++) {
    unsigned tokOffset;
    clang_getSpellingLocation(clang_getTokenLocation(tu, tokens[i]), nullptr,
                              nullptr, nullptr, &tokOffset);

    if (tokOffset < childStartOffset || tokOffset >= childEndOffset) {
      CXString spelling = clang_getTokenSpelling(tu, tokens[i]);
      info.op = clang_getCString(spelling);
      clang_disposeString(spelling);
      info.isPrefix = (tokOffset < childStartOffset);
      break;
    }
  }

  clang_disposeTokens(tu, tokens, numTokens);
  return info;
}

std::string IRBuilder::getCalleeName(CXCursor cursor) {
  auto children = getChildren(cursor);
  if (children.empty())
    return "";

  CXCursor callee = children[0];
  auto kind = clang_getCursorKind(callee);

  if (kind == CXCursor_DeclRefExpr)
    return getCursorSpelling(callee);

  if (kind == CXCursor_UnexposedExpr) {
    for (auto &child : getChildren(callee)) {
      if (clang_getCursorKind(child) == CXCursor_DeclRefExpr)
        return getCursorSpelling(child);
    }
  }

  return getCursorSpelling(callee);
}

// libclang helpers
std::vector<CXCursor> IRBuilder::getChildren(CXCursor cursor) {
  std::vector<CXCursor> children;
  clang_visitChildren(
      cursor,
      [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
        static_cast<std::vector<CXCursor> *>(data)->push_back(child);
        return CXChildVisit_Continue;
      },
      &children);
  return children;
}

std::string IRBuilder::getSourceText(CXCursor cursor) {
  CXSourceRange range = clang_getCursorExtent(cursor);
  CXFile file;
  unsigned startOffset, endOffset;
  clang_getSpellingLocation(clang_getRangeStart(range), &file, nullptr, nullptr,
                            &startOffset);
  clang_getSpellingLocation(clang_getRangeEnd(range), nullptr, nullptr, nullptr,
                            &endOffset);
  if (!file)
    return "";
  return getSourceSlice(file, startOffset, endOffset);
}

std::string IRBuilder::getCursorSpelling(CXCursor cursor) {
  CXString spelling = clang_getCursorSpelling(cursor);
  std::string result = clang_getCString(spelling);
  clang_disposeString(spelling);
  return result;
}

std::string IRBuilder::getTypeSpelling(CXCursor cursor) {
  CXType type = clang_getCursorType(cursor);
  CXString spelling = clang_getTypeSpelling(type);
  std::string result = clang_getCString(spelling);
  clang_disposeString(spelling);
  return result;
}

SourceLoc IRBuilder::getLoc(CXCursor cursor) {
  CXSourceLocation clangLoc = clang_getCursorLocation(cursor);
  CXFile file;
  unsigned line, col;
  clang_getSpellingLocation(clangLoc, &file, &line, &col, nullptr);

  SourceLoc loc;
  loc.line = line;
  loc.col = col;
  if (file) {
    CXString fileName = clang_getFileName(file);
    loc.file = clang_getCString(fileName);
    clang_disposeString(fileName);
  }
  return loc;
}

const std::string &IRBuilder::getFileContent(CXFile file) {
  static const std::string empty;
  if (!file)
    return empty;

  CXString fileName = clang_getFileName(file);
  std::string name = clang_getCString(fileName);
  clang_disposeString(fileName);

  auto it = sourceCache.find(name);
  if (it != sourceCache.end())
    return it->second;

  std::ifstream f(name, std::ios::binary);
  if (!f)
    return empty;
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  return sourceCache.emplace(name, content).first->second;
}

std::string IRBuilder::getSourceSlice(CXFile file, unsigned start,
                                      unsigned end) {
  const std::string &content = getFileContent(file);
  if (start >= content.size() || end > content.size() || start >= end)
    return "";
  return content.substr(start, end - start);
}

bool IRBuilder::getDescendant(CXCursor cursor, CXCursorKind targetKind) {
  struct Ctx {
    CXCursorKind target;
    bool found;
  };
  Ctx ctx{targetKind, false};
  clang_visitChildren(
      cursor,
      [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto *ctx = static_cast<Ctx *>(data);
        if (clang_getCursorKind(child) == ctx->target) {
          ctx->found = true;
          return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
      },
      &ctx);
  return ctx.found;
}

} // namespace pancake
