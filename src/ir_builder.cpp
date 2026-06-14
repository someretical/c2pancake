#include "ir_builder.h"
#include "pancake_ir.h"

#include <cassert>
#include <clang-c/Index.h>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <print>
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

BlockPtr IRBuilder::buildBlockOrWrap(CXCursor cursor) {
  if (clang_getCursorKind(cursor) == CXCursor_CompoundStmt)
    return buildBlock(cursor);
  auto block = std::make_shared<Block>();
  buildStmt(cursor);
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

// TODO replace with optional or expected
std::unique_ptr<Program> IRBuilder::build(const std::string &filename) {
  scanFFIAnnotations(filename);
  index = clang_createIndex(0, 0);

  const char *args[] = {"-std=c11", "-I/usr/include", "-I/usr/local/include"};
  tu =
      clang_parseTranslationUnit(index, filename.c_str(), args, 3, nullptr, 0,
                                 CXTranslationUnit_DetailedPreprocessingRecord);
  if (!tu) {
    std::println("Failed to parse {}", filename);
    return nullptr;
  }

  auto program = std::make_unique<Program>();
  currentProgram = program.get();
  auto root = clang_getTranslationUnitCursor(tu);

  for (auto child : getChildren(root)) {
    if (clang_Location_isInSystemHeader(clang_getCursorLocation(child)))
      continue;

    auto kind = clang_getCursorKind(child);
    auto loc = getLoc(child);

    switch (kind) {
    case CXCursor_FunctionDecl: {
      program->functions.push_back(buildFunction(child));
      break;
    }
    case CXCursor_VarDecl: {
      break;
    }
    case CXCursor_StructDecl: {
      break;
    }
    case CXCursor_TypedefDecl: {
      break;
    }
    case CXCursor_EnumDecl: {
      break;
    }
    default: {
      break;
    }
    }
  }

  // // walk top-level declarations
  // struct Ctx {
  //   IRBuilder *self;
  //   Program *prog;
  // };
  // Ctx ctx{this, program.get()};

  // clang_visitChildren(
  //     root,
  //     [](CXCursor cursor, CXCursor, CXClientData data) -> CXChildVisitResult
  //     {
  //       auto *ctx = static_cast<Ctx *>(data);
  //       if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)))
  //         return CXChildVisit_Continue;

  //       auto kind = clang_getCursorKind(cursor);
  //       if (kind == CXCursor_FunctionDecl) {
  //         auto func = ctx->self->buildFunction(cursor);
  //         if (func)
  //           ctx->prog->functions.push_back(func);
  //       } else if (kind == CXCursor_VarDecl) {
  //         ctx->self->buildVarDecl(cursor, ctx->prog->globals);
  //       } else if (kind == CXCursor_StructDecl) {
  //         ctx->self->buildStructDecl(cursor, ctx->prog->globals);
  //       } else if (kind == CXCursor_TypedefDecl) {
  //         ctx->prog->globals.push_back(std::make_shared<CommentStmt>(
  //             "TODO: Typedef: " + ctx->self->getSourceText(cursor),
  //             ctx->self->getLoc(cursor)));
  //       } else if (kind == CXCursor_EnumDecl) {
  //         ctx->self->buildEnumDecl(cursor, ctx->prog->globals);
  //       }
  //       return CXChildVisit_Continue;
  //     },
  //     &ctx);

  // inject global array inits into main's body
  if (!program->arrayInits.empty()) {
    auto it = std::ranges::find_if(
        program->functions, [](const auto &f) { return f->name == "main"; });
    if (it == program->functions.end()) {
      std::println(
          std::cerr,
          "Error: global array initializers present but no main() found");
    } else {
      auto &mainFunc = *it;
      assert(mainFunc->body);
      auto &stmts = mainFunc->body->stmts;
      stmts.reserve(stmts.size() + program->arrayInits.size());
      stmts.insert(stmts.begin(),
                   std::make_move_iterator(program->arrayInits.begin()),
                   std::make_move_iterator(program->arrayInits.end()));
      program->arrayInits.clear();
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
      auto canonParamType = clang_getCanonicalType(clang_getCursorType(child));
      if (canonParamType.kind == CXType_Record) {
        // get struct name
        auto decl = clang_getTypeDeclaration(canonParamType);
        auto name = clang_getCursorSpelling(decl);
        std::string structName = clang_getCString(name);
        clang_disposeString(name);

        auto stIt = structTypes.find(structName);
        if (stIt != structTypes.end()) {
          p.shape = stIt->second.fieldNames.size();
          structVars[p.name] = structName;
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
  auto funcType = clang_getCursorType(cursor);
  auto canonicalReturn = clang_getCanonicalType(clang_getResultType(funcType));
  if (canonicalReturn.kind == CXType_Void &&
      (stmts.empty() || stmts.back()->kind != StmtKind::Return)) {
    // add return void;
    stmts.push_back(std::make_shared<ReturnStmt>(nullptr, func->loc));
  } else if (canonicalReturn.kind != CXType_Void &&
             (stmts.empty() || stmts.back()->kind != StmtKind::Return)) {
    // add return 0;
    auto endLoc = getEndLoc(cursor);
    stmts.push_back(std::make_shared<ReturnStmt>(
        std::make_shared<IntLitExpr>(0, endLoc), endLoc));
  }

  return func;
}

BlockPtr IRBuilder::buildBlock(CXCursor cursor) {
  auto block = std::make_shared<Block>();
  block->loc = getLoc(cursor);

  for (auto child : getChildren(cursor)) {
    auto stmts = buildStmt(child);
    block->stmts.insert(block->stmts.end(), stmts.begin(), stmts.end());
  }

  return block;
}

std::vector<StmtPtr> IRBuilder::buildStmt(CXCursor cursor) {
  if (clang_Location_isInSystemHeader(clang_getCursorLocation(cursor)))
    return {};

  auto kind = clang_getCursorKind(cursor);
  auto loc = getLoc(cursor);
  std::vector<StmtPtr> resultStmts;

  // check for FFI replacement
  // TODO check over this code
  auto ffiIt = ffiLineReplacements.find(loc.line);
  if (ffiIt != ffiLineReplacements.end()) {
    // TODO make a custom FFI stmt
    resultStmts.push_back(std::make_shared<ExprStmt>(
        std::make_shared<RawExpr>("@" + ffiIt->second + "(0,0,0,0)", loc),
        loc));
    return resultStmts;
  }

  switch (kind) {
    // e.g. int a = 1, b = 2;
  case CXCursor_DeclStmt: {
    for (auto child : getChildren(cursor)) {
      auto stmtsList = buildStmt(child);
      resultStmts.insert(resultStmts.end(), stmtsList.begin(), stmtsList.end());
    }
    break;
  }

  // e.g. int a = 1;
  case CXCursor_VarDecl: {
    // needs to reverse the order of pre statements from expr builder
    auto stmtsList = buildVarDecl(cursor);
    resultStmts.insert(resultStmts.end(), stmtsList.begin(), stmtsList.end());
    break;
  }

  // e.g. { ... }
  case CXCursor_CompoundStmt: {
    auto block = buildBlock(cursor);
    auto blockStmt = std::make_shared<BlockStmt>(block, loc);
    resultStmts.push_back(blockStmt);
    break;
  }

  // e.g. return 0; or just return;
  case CXCursor_ReturnStmt: {
    auto children = getChildren(cursor);
    ExprPtr value = nullptr;
    if (!children.empty()) {
      assert(children.size() == 1);
      auto expr = buildExpr(children[0], CursorContext::ValueContext);
      // needs to reverse the order of pre statements from expr builder
      resultStmts.insert(resultStmts.end(), expr.preStmts.rbegin(),
                         expr.preStmts.rend());
      value = expr.finalExpr;
      // doesn't make sense to include any post statements
    }
    resultStmts.push_back(std::make_shared<ReturnStmt>(value, loc));
    break;
  }
  // case CXCursor_IfStmt: {
  //   auto children = getChildren(cursor);
  //   if (children.size() < 2)
  //     break;

  //   // hoist array loads from condition
  //   ExprPtr cond = hoistArrayLoads(children[0], stmts);
  //   if (!cond)
  //     cond = buildExpr(children[0]);

  //   auto thenBranch = buildBlockOrWrap(children[1]);

  //   BlockPtr elseBranch;
  //   if (children.size() >= 3)
  //     elseBranch = buildBlockOrWrap(children[2]);

  //   stmts.push_back(
  //       std::make_shared<IfStmt>(cond, thenBranch, elseBranch, loc));
  //   break;
  // }
  // case CXCursor_WhileStmt: {
  //   auto children = getChildren(cursor);
  //   if (children.size() < 2)
  //     break;

  //   auto cond = buildExpr(children[0]);
  //   auto body = buildBlockOrWrap(children[1]);

  //   stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
  //   break;
  // }
  // case CXCursor_ForStmt:
  //   buildForStmt(cursor, stmts);
  //   break;

  // case CXCursor_DoStmt: {
  //   // do-while -> body once, then while loop
  //   auto children = getChildren(cursor);
  //   if (children.size() < 2)
  //     break;

  //   CXCursor bodyCursor = children[0];
  //   CXCursor condCursor = children[1];

  //   // emit body once (the "do" part)
  //   if (clang_getCursorKind(bodyCursor) == CXCursor_CompoundStmt) {
  //     auto block = buildBlock(bodyCursor);
  //     for (auto &s : block->stmts)
  //       stmts.push_back(s);
  //   } else {
  //     buildStmt(bodyCursor, stmts);
  //   }

  //   // build while loop (re-parses body cursor)
  //   auto cond = buildExpr(condCursor);
  //   auto body = buildBlockOrWrap(bodyCursor);

  //   stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
  //   break;
  // }

  // case CXCursor_BreakStmt:
  //   stmts.push_back(std::make_shared<BreakStmt>(loc));
  //   break;

  // case CXCursor_ContinueStmt:
  //   stmts.push_back(std::make_shared<ContinueStmt>(loc));
  //   break;

  // case CXCursor_CompoundAssignOperator:
  //   buildCompoundAssign(cursor, stmts);
  //   break;

  // case CXCursor_UnaryOperator: {
  //   auto result = tryBuildIncrDecr(cursor);
  //   if (result) {
  //     stmts.push_back(result);
  //   } else {
  //     pushExprStmt(stmts, cursor);
  //   }
  //   break;
  // }
  // case CXCursor_CallExpr: {
  //   std::string name = getCalleeName(cursor);
  //   if (name == "printf") {
  //     stmts.push_back(std::make_shared<CommentStmt>(
  //         "TODO: printf not available in Pancake - " +
  //         getSourceText(cursor), loc));
  //   } else {
  //     stmts.push_back(std::make_shared<ExprStmt>(buildExpr(cursor), loc));
  //   }
  //   break;
  // }

  // e.g. a + b; a = b; -a, etc
  case CXCursor_BinaryOperator: {
    auto builtExpr = buildBinaryExpr(cursor, CursorContext::AssignmentTarget,
                                     CursorContext::ValueContext);
    // reverse the order of pre statements from expr builder
    resultStmts.insert(resultStmts.end(), builtExpr.preStmts.rbegin(),
                       builtExpr.preStmts.rend());
    resultStmts.push_back(std::make_shared<ExprStmt>(builtExpr.finalExpr, loc));
    resultStmts.insert(resultStmts.end(), builtExpr.postStmts.begin(),
                       builtExpr.postStmts.end());
    break;
  }
  // e.g. a += b, a -= 1, etc
  case CXCursor_CompoundAssignOperator: {
    auto builtExpr = buildCompoundAssignExpr(
        cursor, CursorContext::AssignmentTarget, CursorContext::ValueContext);
    // reverse the order of pre statements from expr builder
    resultStmts.insert(resultStmts.end(), builtExpr.preStmts.rbegin(),
                       builtExpr.preStmts.rend());
    resultStmts.push_back(std::make_shared<ExprStmt>(builtExpr.finalExpr, loc));
    resultStmts.insert(resultStmts.end(), builtExpr.postStmts.begin(),
                       builtExpr.postStmts.end());
    break;
  }
    //   auto children = getChildren(cursor);
    //   std::string opStr = extractBinOp(cursor);
    //   if (opStr == "=" && children.size() == 2) {
    //     // struct field assignment
    //     if (clang_getCursorKind(children[0]) == CXCursor_MemberRefExpr) {
    //       auto memberChildren = getChildren(children[0]);
    //       std::string fieldName = getCursorSpelling(children[0]);
    //       if (!memberChildren.empty()) {
    //         std::string varName = resolveVarName(memberChildren[0]);
    //         int fi = findStructFieldIndex(varName, fieldName);
    //         if (fi >= 0) {
    //           int fc = getStructFieldCount(varName);
    //           auto value = buildExpr(children[1]);
    //           stmts.push_back(std::make_shared<StructFieldAssignStmt>(
    //               varName, fi, fc, value, loc));
    //           return;
    //         }
    //       }
    //     }
    //     // array element assignment
    //     if (clang_getCursorKind(children[0]) == CXCursor_ArraySubscriptExpr)
    //     {
    //       auto arrChildren = getChildren(children[0]);
    //       if (arrChildren.size() == 2) {
    //         std::string arrName = resolveVarName(arrChildren[0]);
    //         auto ait = globalArrays.find(arrName);
    //         if (ait != globalArrays.end()) {
    //           std::string addr =
    //               arrayAddr(ait->second.baseSlot,
    //               getSourceText(arrChildren[1]));
    //           // hoist RHS if it contains array access
    //           ExprPtr srcVar = hoistArrayLoads(children[1], stmts);
    //           if (!srcVar) {
    //             srcVar = buildExpr(children[1]);
    //             // emit store via temp var
    //             std::string valTmp = std::format("av{}", arrayTmpCounter++);
    //             stmts.push_back(std::make_shared<VarDeclStmt>(valTmp, srcVar,
    //                                                           std::nullopt,
    //                                                           loc));
    //             srcVar = std::make_shared<VarRefExpr>(valTmp, loc);
    //           }

    //           // TODO we need to parse the LHS for any array access and so on
    //           as
    //           // well
    //           auto destVar = std::make_shared<RawExpr>(addr, loc);
    //           stmts.push_back(
    //               std::make_shared<MemoryStoreStmt>(srcVar, destVar, loc));
    //           return;
    //         }
    //       }
    //     }
    //     std::string target = getCursorSpelling(children[0]);
    //     // hoist array loads from RHS
    //     auto hoisted = hoistArrayLoads(children[1], stmts);
    //     if (hoisted) {
    //       stmts.push_back(std::make_shared<AssignStmt>(target, hoisted,
    //       loc)); return;
    //     }
    //     // expand ternary in RHS
    //     auto ternaryResult = tryExpandTernary(children[1], stmts);
    //     if (ternaryResult) {
    //       stmts.push_back(
    //           std::make_shared<AssignStmt>(target, ternaryResult, loc));
    //     } else {
    //       auto value = buildExpr(children[1]);
    //       stmts.push_back(std::make_shared<AssignStmt>(target, value, loc));
    //     }
    //   } else {
    //     pushExprStmt(stmts, cursor);
    //   }
    //   break;
    // }
    // case CXCursor_SwitchStmt:
    //   buildSwitchStmt(cursor, stmts);
    //   break;

  default:
    // pushExprStmt(stmts, cursor);
    break;
  }

  return resultStmts;
}

// needs to reverse the order of pre statements from expr builder
std::vector<StmtPtr> IRBuilder::buildVarDecl(CXCursor cursor) {
  std::string name = getCursorSpelling(cursor);
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  auto varType = clang_getCursorType(cursor);

  std::vector<StmtPtr> resultStmts;

  switch (varType.kind) {
  case CXType_ConstantArray: {
    // TODO support multidimensional arrays and arrays of types other than i32
    auto arrSize = clang_getArraySize(varType);
    if (arrSize > 0) {
      ArrayInfo info{nextArraySlot, (int)arrSize};
      globalArrays[name] = info;
      nextArraySlot += (int)arrSize;
      resultStmts.push_back(std::make_shared<CommentStmt>(
          std::format("array {}[{}] -> @base slots {}..={}", name, arrSize,
                      info.baseSlot, info.baseSlot + info.size - 1),
          loc));
      // support multiple levels of initializer lists for multidimensional
      // arrays
    }
    break;
  }

  // structs
  case CXType_Record: {
    // get struct name
    auto canonParamType = clang_getCanonicalType(varType);
    auto decl = clang_getTypeDeclaration(canonParamType);
    auto name_ = clang_getCursorSpelling(decl);
    std::string structName = clang_getCString(name_);
    clang_disposeString(name_);

    auto it = structTypes.find(structName);
    if (it != structTypes.end()) {
      const auto &info = it->second;
      auto fieldCount = info.fieldNames.size();
      structVars[name] = structName;

      // TODO support struct initialization with initializer list, e.g. struct S
      // { int x, y; }; struct S s = {1, 2};

      resultStmts.push_back(
          std::make_shared<VarDeclStmt>(name, nullptr, fieldCount, loc));
      break;
    }
  }

  case CXType_Int: {
    assert(children.size() <= 1);
    ExprPtr init = nullptr;
    if (!children.empty()) {
      auto stmtList = buildExpr(children[0], CursorContext::ValueContext);
      resultStmts.insert(resultStmts.end(), stmtList.preStmts.rbegin(),
                         stmtList.preStmts.rend());
      resultStmts.push_back(std::make_shared<VarDeclStmt>(
          name, stmtList.finalExpr, std::nullopt, loc));
      resultStmts.insert(resultStmts.end(), stmtList.postStmts.begin(),
                         stmtList.postStmts.end());
    } else {
      resultStmts.push_back(
          std::make_shared<VarDeclStmt>(name, nullptr, 1, loc));
    }
    break;
  }

  // TODO support other primitive types
  default: {
    break;
  }
  }

  return resultStmts;

  // // array declaration
  // CXType varType = clang_getCursorType(cursor);
  // if (varType.kind == CXType_ConstantArray) {
  //   long long arrSize = clang_getArraySize(varType);
  //   if (arrSize > 0) {

  //     stmts.push_back(std::make_shared<CommentStmt>(
  //         "array " + name + "[" + std::to_string(arrSize) + "] → @base
  //         slots " +
  //             std::to_string(info.baseSlot) + ".." +
  //             std::to_string(info.baseSlot + info.size - 1),
  //         loc));

  //     auto &target = insideFunction ? stmts : currentProgram->arrayInits;
  //     for (auto &child : children) {
  //       if (clang_getCursorKind(child) == CXCursor_InitListExpr) {
  //         auto initChildren = getChildren(child);
  //         for (int i = 0; i < (int)initChildren.size() && i < (int)arrSize;
  //              i++) {
  //           target.push_back(std::make_shared<ExprStmt>(
  //               std::make_shared<RawExpr>("st " + arrayAddr(info.baseSlot +
  //               i) +
  //                                             ", " +
  //                                             getSourceText(initChildren[i]),
  //                                         loc),
  //               loc));
  //         }
  //       }
  //     }
  //     return {};
  //   }
  // }

  // // struct variable
  // CXType canonType = clang_getCanonicalType(varType);
  // if (canonType.kind == CXType_Record) {
  //   CXString typeSpelling = clang_getTypeSpelling(canonType);
  //   std::string structName =
  //   stripStructPrefix(clang_getCString(typeSpelling));
  //   clang_disposeString(typeSpelling);

  //   auto it = structTypes.find(structName);
  //   if (it != structTypes.end()) {
  //     const auto &info = it->second;
  //     int fieldCount = (int)info.fieldNames.size();

  //     structVars[name] = structName;

  //     ExprPtr init;
  //     if (!children.empty()) {
  //       auto lastChild = children.back();
  //       auto lastKind = clang_getCursorKind(lastChild);

  //       if (lastKind == CXCursor_InitListExpr) {
  //         auto initChildren = getChildren(lastChild);
  //         std::vector<ExprPtr> fieldExprs;
  //         for (auto &ic : initChildren)
  //           fieldExprs.push_back(buildExpr(ic));
  //         init = std::make_shared<StructLitExpr>(fieldExprs, loc);
  //       } else {
  //         init = buildExpr(lastChild);
  //       }
  //     }

  //     stmts.push_back(
  //         std::make_shared<VarDeclStmt>(name, init, fieldCount, loc));
  //     return {};
  //   }
  // }

  // ExprPtr init;
  // std::optional<int> shape;

  // if (!children.empty()) {
  //   // hoist array loads from initializer
  //   auto hoisted = hoistArrayLoads(children.back(), stmts);
  //   if (hoisted) {
  //     stmts.push_back(
  //         std::make_shared<VarDeclStmt>(name, hoisted, std::nullopt, loc));
  //     return {};
  //   }

  //   // expand ternary initializer
  //   if (getDescendant(cursor, CXCursor_ConditionalOperator)) {
  //     auto ternaryResult = tryExpandTernary(children.back(), stmts);
  //     if (ternaryResult) {
  //       stmts.push_back(std::make_shared<VarDeclStmt>(name, ternaryResult,
  //                                                     std::nullopt, loc));
  //       return {};
  //     }
  //   }

  //   // function call in initializer needs shape annotation
  //   if (getDescendant(cursor, CXCursor_CallExpr))
  //     shape = 1;

  //   init = buildExpr(children.back());

  //   // emit unsupported initializers as comments to avoid invalid Pancake
  //   if (init && init->kind == ExprKind::Raw) {
  //     auto &raw = static_cast<RawExpr &>(*init);
  //     if (raw.text.find("TODO") != std::string::npos) {
  //       stmts.push_back(std::make_shared<CommentStmt>(
  //           "TODO: " + getSourceText(cursor), loc));
  //       return {};
  //     }
  //   }
  // }

  // stmts.push_back(std::make_shared<VarDeclStmt>(name, init, shape, loc));
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

std::vector<StmtPtr> IRBuilder::buildForStmt(CXCursor cursor) {
  // auto loc = getLoc(cursor);
  // auto parts = classifyForChildren(cursor);

  // if (parts.hasInit)
  //   buildStmt(parts.init, stmts);

  // ExprPtr cond = parts.hasCond ? buildExpr(parts.condition)
  //                              : std::make_shared<IntLitExpr>(1, loc);

  // auto body = buildBlockOrWrap(parts.body);

  // if (parts.hasUpdate)
  //   buildForUpdate(parts.update, body->stmts);

  // stmts.push_back(std::make_shared<WhileStmt>(cond, body, loc));
  return {};
}

void IRBuilder::buildForUpdate(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  // auto kind = clang_getCursorKind(cursor);

  // if (kind == CXCursor_UnaryOperator) {
  //   auto result = tryBuildIncrDecr(cursor);
  //   if (result) {
  //     stmts.push_back(result);
  //     return;
  //   }
  // }
  // if (kind == CXCursor_CompoundAssignOperator) {
  //   buildCompoundAssign(cursor, stmts);
  //   return;
  // }
  // if (kind == CXCursor_BinaryOperator) {
  //   auto children = getChildren(cursor);
  //   std::string opStr = extractBinOp(cursor);
  //   if (opStr == "=" && children.size() == 2) {
  //     std::string target = getCursorSpelling(children[0]);
  //     auto value = buildExpr(children[1]);
  //     stmts.push_back(
  //         std::make_shared<AssignStmt>(target, value, getLoc(cursor)));
  //     return;
  //   }
  // }
  // pushExprStmt(stmts, cursor);
}

bool IRBuilder::cursorHasSideEffects(CXCursor cursor) {
  auto kind = clang_getCursorKind(cursor);

  if (kind == CXCursor_CallExpr || kind == CXCursor_CompoundAssignOperator)
    return true;

  if (kind == CXCursor_BinaryOperator) {

    auto op = clang_getCursorBinaryOperatorKind(cursor);
    if (op == CXBinaryOperator_Assign)
      return true;
  }

  if (kind == CXCursor_UnaryOperator) {
    auto op = clang_getCursorUnaryOperatorKind(cursor);
    if (op == CXUnaryOperator_PostInc || op == CXUnaryOperator_PostDec ||
        op == CXUnaryOperator_PreInc || op == CXUnaryOperator_PreDec)
      return true;
  }

  // treat reads of volatile variables as side-effects
  if (kind == CXCursor_DeclRefExpr) {
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

std::vector<StmtPtr> IRBuilder::buildStructDecl(CXCursor cursor) {
  // std::string name = getCursorSpelling(cursor);

  // StructInfo info;
  // for (auto &child : getChildren(cursor)) {
  //   if (clang_getCursorKind(child) == CXCursor_FieldDecl)
  //     info.fieldNames.push_back(getCursorSpelling(child));
  // }

  // if (!name.empty() && !info.fieldNames.empty())
  //   structTypes[name] = info;
  return {};
}

std::vector<StmtPtr> IRBuilder::buildEnumDecl(CXCursor cursor) {
  // auto loc = getLoc(cursor);
  // for (auto &child : getChildren(cursor)) {
  //   if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl) {
  //     std::string name = getCursorSpelling(child);
  //     int64_t value = clang_getEnumConstantDeclValue(child);
  //     stmts.push_back(std::make_shared<DefineStmt>(name, value, loc));
  //   }
  // }
  return {};
}

// switch -> nested if/else

std::vector<StmtPtr> IRBuilder::buildSwitchStmt(CXCursor cursor) {
  // auto loc = getLoc(cursor);
  // auto children = getChildren(cursor);

  // if (children.size() < 2) {
  //   stmts.push_back(std::make_shared<CommentStmt>(
  //       "TODO: unsupported switch: " + getSourceText(cursor), loc));
  //   return;
  // }

  // auto switchExpr = buildExpr(children[0]);
  // std::string switchVar = "sw" + std::to_string(switchVarCounter++);

  // stmts.push_back(
  //     std::make_shared<VarDeclStmt>(switchVar, switchExpr, std::nullopt,
  //     loc));

  // // collect case groups
  // struct CaseGroup {
  //   std::vector<ExprPtr> values; // empty means default
  //   std::vector<StmtPtr> body;
  //   SourceLoc loc;
  // };
  // std::vector<CaseGroup> groups;

  // auto bodyChildren = getChildren(children[1]);

  // // recursively unwrap nested case/default labels
  // struct LabelInfo {
  //   std::vector<ExprPtr> values;
  //   CXCursor bodyCursor;
  //   bool hasBody = false;
  // };

  // std::function<LabelInfo(CXCursor)> unwrapLabels =
  //     [&](CXCursor c) -> LabelInfo {
  //   LabelInfo info;
  //   auto k = clang_getCursorKind(c);
  //   auto ch = getChildren(c);

  //   if (k == CXCursor_CaseStmt) {
  //     if (ch.size() >= 1)
  //       info.values.push_back(buildExpr(ch[0]));
  //     if (ch.size() >= 2) {
  //       auto subKind = clang_getCursorKind(ch[1]);
  //       if (subKind == CXCursor_CaseStmt || subKind ==
  //       CXCursor_DefaultStmt)
  //       {
  //         auto sub = unwrapLabels(ch[1]);
  //         for (auto &v : sub.values)
  //           info.values.push_back(v);
  //         info.bodyCursor = sub.bodyCursor;
  //         info.hasBody = sub.hasBody;
  //       } else {
  //         info.bodyCursor = ch[1];
  //         info.hasBody = true;
  //       }
  //     }
  //   } else if (k == CXCursor_DefaultStmt) {
  //     if (!ch.empty()) {
  //       auto subKind = clang_getCursorKind(ch[0]);
  //       if (subKind == CXCursor_CaseStmt || subKind ==
  //       CXCursor_DefaultStmt)
  //       {
  //         auto sub = unwrapLabels(ch[0]);
  //         for (auto &v : sub.values)
  //           info.values.push_back(v);
  //         info.bodyCursor = sub.bodyCursor;
  //         info.hasBody = sub.hasBody;
  //       } else {
  //         info.bodyCursor = ch[0];
  //         info.hasBody = true;
  //       }
  //     }
  //   }
  //   return info;
  // };

  // for (auto &child : bodyChildren) {
  //   auto k = clang_getCursorKind(child);

  //   if (k == CXCursor_CaseStmt || k == CXCursor_DefaultStmt) {
  //     auto info = unwrapLabels(child);

  //     CaseGroup group;
  //     group.values = info.values;
  //     group.loc = getLoc(child);

  //     if (info.hasBody) {
  //       auto innerKind = clang_getCursorKind(info.bodyCursor);
  //       if (innerKind == CXCursor_CompoundStmt) {
  //         auto block = buildBlock(info.bodyCursor);
  //         for (auto &s : block->stmts)
  //           group.body.push_back(s);
  //       } else if (innerKind == CXCursor_BreakStmt) {
  //         // skip
  //       } else {
  //         buildStmt(info.bodyCursor, group.body);
  //       }
  //     }

  //     groups.push_back(group);
  //   } else if (k == CXCursor_BreakStmt) {
  //     // skip
  //   } else {
  //     if (!groups.empty())
  //       buildStmt(child, groups.back().body);
  //   }
  // }

  // // remove trailing breaks from each group
  // for (auto &g : groups) {
  //   while (!g.body.empty() && g.body.back()->kind == StmtKind::Break)
  //     g.body.pop_back();
  // }

  // // find default group
  // int defaultIdx = -1;
  // for (int i = 0; i < (int)groups.size(); i++) {
  //   if (groups[i].values.empty()) {
  //     defaultIdx = i;
  //     break;
  //   }
  // }

  // BlockPtr elseBranch;
  // if (defaultIdx >= 0) {
  //   elseBranch = std::make_shared<Block>();
  //   elseBranch->stmts = groups[defaultIdx].body;
  // }

  // // build if/else chain back to front
  // StmtPtr result;
  // for (int i = (int)groups.size() - 1; i >= 0; i--) {
  //   if (i == defaultIdx)
  //     continue;

  //   auto &g = groups[i];

  //   // build condition: switchVar == val1 || switchVar == val2 || ...
  //   ExprPtr cond;
  //   for (auto &val : g.values) {
  //     auto eq = std::make_shared<BinaryExpr>(
  //         BinOp::Eq, std::make_shared<VarRefExpr>(switchVar, g.loc), val,
  //         g.loc);
  //     if (!cond) {
  //       cond = eq;
  //     } else {
  //       cond = std::make_shared<BinaryExpr>(BinOp::Or, cond, eq, g.loc);
  //     }
  //   }

  //   if (!cond)
  //     continue;

  //   auto thenBlock = std::make_shared<Block>();
  //   thenBlock->stmts = g.body;

  //   auto ifStmt = std::make_shared<IfStmt>(cond, thenBlock, elseBranch,
  //   g.loc);

  //   elseBranch = std::make_shared<Block>();
  //   elseBranch->stmts.push_back(ifStmt);
  // }

  // if (elseBranch && !elseBranch->stmts.empty()) {
  //   for (auto &s : elseBranch->stmts)
  //     stmts.push_back(s);
  // }
  return {};
}

// ternary expansion (cond ? a : b -> tmp var + if/else)
IRBuilder::BuiltExpression
IRBuilder::tryExpandTernary(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  // auto kind = clang_getCursorKind(cursor);
  // if (kind == CXCursor_UnexposedExpr || kind == CXCursor_ParenExpr) {
  //   auto children = getChildren(cursor);
  //   if (!children.empty())
  //     return tryExpandTernary(children[0], stmts);
  //   return nullptr;
  // }

  // if (kind != CXCursor_ConditionalOperator)
  //   return nullptr;

  // auto loc = getLoc(cursor);
  // auto children = getChildren(cursor);
  // if (children.size() < 3)
  //   return nullptr;

  // std::string tmpVar = "tn" + std::to_string(ternaryVarCounter++);

  // stmts.push_back(std::make_shared<VarDeclStmt>(
  //     tmpVar, std::make_shared<IntLitExpr>(0, loc), std::nullopt, loc));

  // auto cond = buildExpr(children[0]);

  // // then branch (may contain nested ternary)
  // auto thenBlock = std::make_shared<Block>();
  // auto nestedTrue = tryExpandTernary(children[1], thenBlock->stmts);
  // if (nestedTrue) {
  //   thenBlock->stmts.push_back(
  //       std::make_shared<AssignStmt>(tmpVar, nestedTrue, loc));
  // } else {
  //   thenBlock->stmts.push_back(
  //       std::make_shared<AssignStmt>(tmpVar, buildExpr(children[1]), loc));
  // }

  // // else branch (may contain nested ternary)
  // auto elseBlock = std::make_shared<Block>();
  // auto nestedFalse = tryExpandTernary(children[2], elseBlock->stmts);
  // if (nestedFalse) {
  //   elseBlock->stmts.push_back(
  //       std::make_shared<AssignStmt>(tmpVar, nestedFalse, loc));
  // } else {
  //   elseBlock->stmts.push_back(
  //       std::make_shared<AssignStmt>(tmpVar, buildExpr(children[2]), loc));
  // }

  // stmts.push_back(std::make_shared<IfStmt>(cond, thenBlock, elseBlock,
  // loc));

  // return std::make_shared<VarRefExpr>(tmpVar, loc);
  return {};
}

IRBuilder::BuiltExpression IRBuilder::buildExpr(CXCursor cursor,
                                                enum CursorContext ctx) {
  auto kind = clang_getCursorKind(cursor);
  auto loc = getLoc(cursor);
  IRBuilder::BuiltExpression result;

  switch (kind) {
  case CXCursor_IntegerLiteral: {
    result = buildIntLit(cursor, ctx);
    break;
  }

  case CXCursor_FloatingLiteral:
  case CXCursor_StringLiteral:
  case CXCursor_CharacterLiteral: {
    result.finalExpr = std::make_shared<RawExpr>(getSourceText(cursor), loc);
    break;
  }

  case CXCursor_DeclRefExpr: {
    result.finalExpr =
        std::make_shared<VarRefExpr>(getCursorSpelling(cursor), loc);
    break;
  }

  case CXCursor_BinaryOperator: {
    CursorContext lhsCtx, rhsCtx;
    switch (ctx) {
    case CursorContext::AssignmentTarget: {
      lhsCtx = CursorContext::AssignmentTarget;
      rhsCtx = CursorContext::ValueContext;
      break;
    }
    case CursorContext::ValueContext: {
      lhsCtx = CursorContext::ValueContext;
      rhsCtx = CursorContext::ValueContext;
      break;
    }
    case CursorContext::AddressContext: {
      lhsCtx = CursorContext::AddressContext;
      rhsCtx = CursorContext::AddressContext;
      break;
    }
    }
    result = buildBinaryExpr(cursor, lhsCtx, rhsCtx);
    break;
  }

  case CXCursor_CompoundAssignOperator: {
    result = buildCompoundAssignExpr(cursor);
    break;
  }

  case CXCursor_UnaryOperator: {
    result = buildUnaryExpr(cursor);
    break;
  }

  case CXCursor_CallExpr: {
    // TODO
    result = buildCallExpr(cursor);
    break;
  }

  case CXCursor_ParenExpr:
  case CXCursor_UnexposedExpr:
  case CXCursor_CStyleCastExpr: {
    auto children = getChildren(cursor);
    assert(children.size() <= 1);
    if (!children.empty()) {
      result = buildExpr(children[0], CursorContext::ValueContext);
    } else {
      // not sure when this branch is reached....
      result.finalExpr = std::make_shared<RawExpr>(getSourceText(cursor), loc);
    }
    break;
  }

  case CXCursor_ArraySubscriptExpr: {
    result = buildArraySubscriptExpr(cursor, stmts);
    break;
  }

  case CXCursor_MemberRefExpr: {
    // TODO
    break;
  }

  case CXCursor_ConditionalOperator: {
    // TODO
    break;
  }

  case CXCursor_InitListExpr: {
    // TODO
    break;
  }
    // // this only handles the TRIVIAL case!
    // // For nested index access and so on, see hoistArrayLoads
    // case CXCursor_ArraySubscriptExpr: {
    //   auto arrChildren = getChildren(cursor);
    //   if (arrChildren.size() == 2) {
    //     std::string arrName = resolveVarName(arrChildren[0]);
    //     auto ait = globalArrays.find(arrName);
    //     if (ait != globalArrays.end()) {
    //       std::string addr =
    //           arrayAddr(ait->second.baseSlot, getSourceText(arrChildren[1]));
    //       return std::make_shared<RawExpr>("lds 1 " + addr, loc);
    //     }
    //   }
    //   return std::make_shared<RawExpr>(
    //       "/* TODO: Array access - " + getSourceText(cursor) + " */", loc);
    // }

    // case CXCursor_MemberRefExpr: {
    //   auto memberChildren = getChildren(cursor);
    //   std::string fieldName = getCursorSpelling(cursor);
    //   if (!memberChildren.empty()) {
    //     std::string varName = resolveVarName(memberChildren[0]);
    //     int fi = findStructFieldIndex(varName, fieldName);
    //     if (fi >= 0)
    //       return std::make_shared<FieldAccessExpr>(varName, fi, loc);
    //   }
    //   return std::make_shared<RawExpr>(getSourceText(cursor), loc);
    // }

    // case CXCursor_ConditionalOperator:
    //   return std::make_shared<RawExpr>(getSourceText(cursor), loc);

    // case CXCursor_InitListExpr: {
    //   auto children = getChildren(cursor);
    //   std::string result = "<";
    //   for (size_t i = 0; i < children.size(); i++) {
    //     if (i > 0)
    //       result += ", ";
    //     result += getSourceText(children[i]);
    //   }
    //   result += ">";
    //   return std::make_shared<RawExpr>(result, loc);
    // }
    // case CXCursor_CompoundAssignOperator:
    //   return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  default: {
    result.finalExpr = std::make_shared<RawExpr>(getSourceText(cursor), loc);
    result.preStmts.insert(
        result.preStmts.end(),
        std::make_shared<CommentStmt>(
            "Unsupported expression: " + getSourceText(cursor), loc));
    break;
  }
  }

  return result;
}

IRBuilder::BuiltExpression
IRBuilder::buildBinaryExpr(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  IRBuilder::BuiltExpression result;
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  assert(children.size() == 2);
  auto lhs = children[0];
  auto rhs = children[1];
  auto lhsKind = clang_getCursorKind(lhs);
  auto rhsKind = clang_getCursorKind(rhs);
  auto op = clang_getCursorBinaryOperatorKind(cursor);
  switch (op) {
  case CXBinaryOperator_Comma: {
    auto lhsResult = buildExpr(lhs, stmts);
    auto rhsResult = buildExpr(rhs, stmts);

    result.preStmts.insert(result.preStmts.end(), rhsResult.preStmts.begin(),
                           rhsResult.preStmts.end());
    // finish processing the left operand ENTIRELY
    result.postStmts.insert(result.postStmts.end(), lhsResult.postStmts.begin(),
                            lhsResult.postStmts.end());
    result.preStmts.push_back(
        std::make_shared<ExprStmt>(lhsResult.finalExpr, loc));
    result.preStmts.insert(result.preStmts.end(), lhsResult.preStmts.begin(),
                           lhsResult.preStmts.end());

    // final result is just the rhs before any post statements
    result.finalExpr = rhsResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(), rhsResult.postStmts.begin(),
                            rhsResult.postStmts.end());

    break;
  }
  case CXBinaryOperator_LT:
  case CXBinaryOperator_GT:
  case CXBinaryOperator_LE:
  case CXBinaryOperator_GE:
  case CXBinaryOperator_EQ:
  case CXBinaryOperator_NE:
  case CXBinaryOperator_LAnd:
  case CXBinaryOperator_LOr: {
    auto getPancakeBinOp = [](CXBinaryOperatorKind op) {
      switch (op) {
      case CXBinaryOperator_LT:
        return BinOp::Lt;
      case CXBinaryOperator_GT:
        return BinOp::Gt;
      case CXBinaryOperator_LE:
        return BinOp::Le;
      case CXBinaryOperator_GE:
        return BinOp::Ge;
      case CXBinaryOperator_EQ:
        return BinOp::Eq;
      case CXBinaryOperator_NE:
        return BinOp::Ne;
      case CXBinaryOperator_LAnd:
        return BinOp::And;
      case CXBinaryOperator_LOr:
        return BinOp::Or;
      default:
        assert(false && "Invalid binary operator");
      }
    };

    auto lhsResult = buildExpr(lhs, stmts);
    auto rhsResult = buildExpr(rhs, stmts);
    result.preStmts.insert(result.preStmts.end(), rhsResult.preStmts.begin(),
                           rhsResult.preStmts.end());
    result.preStmts.insert(result.preStmts.end(), lhsResult.preStmts.begin(),
                           lhsResult.preStmts.end());
    result.finalExpr = std::make_shared<BinaryExpr>(
        getPancakeBinOp(op), lhsResult.finalExpr, rhsResult.finalExpr, loc);
    result.postStmts.insert(result.postStmts.end(), lhsResult.postStmts.begin(),
                            lhsResult.postStmts.end());
    result.postStmts.insert(result.postStmts.end(), rhsResult.postStmts.begin(),
                            rhsResult.postStmts.end());
    break;
  }
  case CXBinaryOperator_Assign:
  case CXBinaryOperator_Mul:
  case CXBinaryOperator_Add:
  case CXBinaryOperator_Sub:
  case CXBinaryOperator_Shl:
  case CXBinaryOperator_Shr:
  case CXBinaryOperator_And:
  case CXBinaryOperator_Xor:
  case CXBinaryOperator_Or: {
    auto getPancakeBinOp = [](CXBinaryOperatorKind op) {
      switch (op) {
      case CXBinaryOperator_Mul:
      case CXBinaryOperator_MulAssign:
        return BinOp::Mul;
      case CXBinaryOperator_Add:
      case CXBinaryOperator_AddAssign:
        return BinOp::Add;
      case CXBinaryOperator_Sub:
      case CXBinaryOperator_SubAssign:
        return BinOp::Sub;
      case CXBinaryOperator_Shl:
      case CXBinaryOperator_ShlAssign:
        return BinOp::Shl;
      case CXBinaryOperator_Shr:
      case CXBinaryOperator_ShrAssign:
        return BinOp::Shr;
      case CXBinaryOperator_And:
      case CXBinaryOperator_AndAssign:
        return BinOp::BitwiseAnd;
      case CXBinaryOperator_Or:
      case CXBinaryOperator_OrAssign:
        return BinOp::BitwiseOr;
      case CXBinaryOperator_Xor:
      case CXBinaryOperator_XorAssign:
        return BinOp::BitwiseXor;
      default:
        assert(false && "Invalid binary operator");
      }
    };

    auto lhsResult = buildExpr(lhs, stmts);
    auto rhsResult = buildExpr(rhs, stmts);

    // pre statement insertion must be in REVERSE order
    if (op == CXBinaryOperator_Assign) {
      result.preStmts.push_back(std::make_shared<AssignStmt>(
          lhsResult.finalExpr, rhsResult.finalExpr, loc));
    } else {
      result.preStmts.push_back(std::make_shared<ExprStmt>(
          std::make_shared<BinaryExpr>(getPancakeBinOp(op), lhsResult.finalExpr,
                                       rhsResult.finalExpr, loc),
          loc));
    }
    result.preStmts.insert(result.preStmts.end(), rhsResult.preStmts.begin(),
                           rhsResult.preStmts.end());
    result.preStmts.insert(result.preStmts.end(), lhsResult.preStmts.begin(),
                           lhsResult.preStmts.end());
    result.finalExpr = lhsResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(), rhsResult.postStmts.begin(),
                            rhsResult.postStmts.end());
    result.postStmts.insert(result.postStmts.end(), lhsResult.postStmts.begin(),
                            lhsResult.postStmts.end());
    break;
  }
  case CXBinaryOperator_Div:
  case CXBinaryOperator_Rem: {
    std::string opName = (op == CXBinaryOperator_Div) ? "Division" : "Modulo";
    result.finalExpr = std::make_shared<RawExpr>(
        std::format(
            "/* TODO: {} not available in Pancake at the moment - {} */",
            opName, getSourceText(cursor)),
        loc);
    break;
  }
  default: {
    assert(false && "Invalid binary operator passed to buildBinaryExpr");
  }
  }
  return result;
}

IRBuilder::BuiltExpression
IRBuilder::buildCompoundAssignExpr(CXCursor cursor,
                                   std::vector<StmtPtr> &stmts) {
  IRBuilder::BuiltExpression result;
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  assert(children.size() == 2);
  auto lhs = children[0];
  auto rhs = children[1];
  auto lhsKind = clang_getCursorKind(lhs);
  auto rhsKind = clang_getCursorKind(rhs);
  auto op = clang_getCursorBinaryOperatorKind(cursor);
  auto getPancakeBinOp = [](CXBinaryOperatorKind op) {
    switch (op) {
    case CXBinaryOperator_MulAssign:
      return BinOp::Mul;
    case CXBinaryOperator_AddAssign:
      return BinOp::Add;
    case CXBinaryOperator_SubAssign:
      return BinOp::Sub;
    case CXBinaryOperator_ShlAssign:
      return BinOp::Shl;
    case CXBinaryOperator_ShrAssign:
      return BinOp::Shr;
    case CXBinaryOperator_AndAssign:
      return BinOp::BitwiseAnd;
    case CXBinaryOperator_OrAssign:
      return BinOp::BitwiseOr;
    case CXBinaryOperator_XorAssign:
      return BinOp::BitwiseXor;
    default:
      assert(false && "Invalid compound assignment operator");
    };
  };

  // LHS must only be evaluated ONCE, so we need to hoist it into a temporary
  auto lhsResult = buildExpr(lhs, stmts);

  // array subscripts are complicated
  /*
  arr[i++] += 1;
  ---
  becomes
  ---
  tmp_index = i;
  i = i + 1;

  arr[tmp_index] = arr[tmp_index] + 1;
  ---
  it should NOT become
  ---
  arr[i++] = arr[i++] + 1;
  ---
  i.e. the index expression is the one which should be stored in the tmpvar
  */

  auto tmpVar =
      std::format("compound_assignment_tmp{}", compoundAssignTmpCounter++);
  switch (lhsResult.finalExpr->kind) {
  case ExprKind::ArrayLoad: {
    break;
  }
  case ExprKind::MemoryLoad: {
    break;
  }
  // TODO handle struct field assignment
  default: {
  }
  }

  // TODO

  return result;
}

IRBuilder::BuiltExpression
IRBuilder::buildArraySubscriptExpr(CXCursor cursor,
                                   std::vector<StmtPtr> &stmts) {
  IRBuilder::BuiltExpression result;
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  assert(children.size() == 2);
  auto base = children[0];
  auto index = children[1];
  auto baseKind = clang_getCursorKind(base);
  auto indexKind = clang_getCursorKind(index);

  auto baseResult = buildExpr(base, stmts);
  auto indexResult = buildExpr(index, stmts);
  result.preStmts.insert(result.preStmts.end(), indexResult.preStmts.begin(),
                         indexResult.preStmts.end());
  result.preStmts.insert(result.preStmts.end(), baseResult.preStmts.begin(),
                         baseResult.preStmts.end());

  // get array name
  auto arrName = getCursorSpelling(base);
  auto arrInfoIt = globalArrays.find(arrName);
  if (arrInfoIt == globalArrays.end()) {
    result.finalExpr = std::make_shared<RawExpr>(
        std::format("{}; // couldn't find array name", getSourceText(cursor)));
  } else {
    auto baseSlot = arrInfoIt->second.baseSlot;
    auto idxExpr = indexResult.finalExpr;
    auto stepExpr = std::make_shared<BytesInWordExpr>();
    result.finalExpr = std::make_shared<ArrayLoadExpr>(
        arrInfoIt->second.baseSlot,
        std::make_shared<ArrayIndexExpr>(idxExpr, stepExpr, loc), loc);
  }

  result.postStmts.insert(result.postStmts.end(), baseResult.postStmts.begin(),
                          baseResult.postStmts.end());
  result.postStmts.insert(result.postStmts.end(), indexResult.postStmts.begin(),
                          indexResult.postStmts.end());
  return result;
}

IRBuilder::BuiltExpression
IRBuilder::buildUnaryExpr(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  IRBuilder::BuiltExpression result;
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  assert(children.size() == 1);
  auto op = clang_getCursorUnaryOperatorKind(cursor);
  switch (op) {
  /** Postfix increment operator. */
  case CXUnaryOperator_PostInc: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr = operandResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());

    // what the fuck does this even work?
    result.postStmts.push_back(std::make_shared<AssignStmt>(
        operandResult.finalExpr,
        std::make_shared<BinaryExpr>(BinOp::Add, operandResult.finalExpr,
                                     std::make_shared<IntLitExpr>(1, loc),
                                     loc)));
    break;
  }
  /** Postfix decrement operator. */
  case CXUnaryOperator_PostDec: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr = operandResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());

    result.postStmts.push_back(std::make_shared<AssignStmt>(
        operandResult.finalExpr,
        std::make_shared<BinaryExpr>(BinOp::Sub, operandResult.finalExpr,
                                     std::make_shared<IntLitExpr>(1, loc),
                                     loc)));
    break;
  }
  /** Prefix increment operator. */
  case CXUnaryOperator_PreInc: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr = operandResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());

    result.preStmts.insert(result.preStmts.begin(),
                           std::make_shared<AssignStmt>(
                               operandResult.finalExpr,
                               std::make_shared<BinaryExpr>(
                                   BinOp::Add, operandResult.finalExpr,
                                   std::make_shared<IntLitExpr>(1, loc), loc)));
    break;
  }
  /** Prefix decrement operator. */
  case CXUnaryOperator_PreDec: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr = operandResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());

    result.preStmts.insert(result.preStmts.begin(),
                           std::make_shared<AssignStmt>(
                               operandResult.finalExpr,
                               std::make_shared<BinaryExpr>(
                                   BinOp::Sub, operandResult.finalExpr,
                                   std::make_shared<IntLitExpr>(1, loc), loc)));
    break;
  }
  /** Address of operator. */
  case CXUnaryOperator_AddrOf: {
    // TODO this is very tricky to handle correctly, as it can be applied to a
    // wide variety of expressions
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr = operandResult.finalExpr;
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());
    break;
  }
  /** Dereference operator. */
  case CXUnaryOperator_Deref: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());

    result.finalExpr =
        std::make_shared<MemoryLoadExpr>(1, operandResult.finalExpr, loc);
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());
    break;
  }
  /** Plus operator. */
  case CXUnaryOperator_Plus: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    // 0 + x
    result.finalExpr = std::make_shared<BinaryExpr>(
        BinOp::Add, std::make_shared<IntLitExpr>(0, loc),
        operandResult.finalExpr, loc);
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());

    break;
  }
  /** Minus operator. */
  case CXUnaryOperator_Minus: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    // 0 - x
    result.finalExpr = std::make_shared<BinaryExpr>(
        BinOp::Sub, std::make_shared<IntLitExpr>(0, loc),
        operandResult.finalExpr, loc);
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());
    break;
  }
  /** Not (bitwise) operator. */
  case CXUnaryOperator_Not: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    // transform into x ^ -1 (assuming 2s complement)
    result.finalExpr = std::make_shared<BinaryExpr>(
        BinOp::BitwiseXor, operandResult.finalExpr,
        std::make_shared<IntLitExpr>(-1, loc), loc);
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());
    break;
  }
  /** LNot (logical NOT operator. */
  case CXUnaryOperator_LNot: {
    auto operandResult = buildExpr(children[0], stmts);
    result.preStmts.insert(result.preStmts.end(),
                           operandResult.preStmts.begin(),
                           operandResult.preStmts.end());
    result.finalExpr =
        std::make_shared<UnaryExpr>(UnaryOp::Not, operandResult.finalExpr, loc);
    result.postStmts.insert(result.postStmts.end(),
                            operandResult.postStmts.begin(),
                            operandResult.postStmts.end());
    break;
  }
  /** "__real expr" operator. */
  case CXUnaryOperator_Real:
  /** "__imag expr" operator. */
  case CXUnaryOperator_Imag:
  /** __extension__ marker operator. */
  case CXUnaryOperator_Extension: {
    result.finalExpr = std::make_shared<RawExpr>(getSourceText(cursor), loc);
    break;
  }
  default: {
    result.finalExpr = std::make_shared<RawExpr>(getSourceText(cursor), loc);
  }
  }

  return result;
}

IRBuilder::BuiltExpression
IRBuilder::buildCallExpr(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  auto loc = getLoc(cursor);
  auto children = getChildren(cursor);
  assert(!children.empty());

  // pre statement list must be put in in reverse (it will be reversed later
  // to produce the correct order)
  std::vector<IRBuilder::BuiltExpression> builtArgs;
  for (size_t i = 1; i < children.size(); i++) {
    builtArgs.push_back(buildExpr(children[i], stmts));
  }

  // if (children.empty())
  //   return std::make_shared<RawExpr>(getSourceText(cursor), loc);

  // std::string callee = getCalleeName(cursor);

  // if (callee == "printf") {
  //   return std::make_shared<RawExpr>(
  //       "/* TODO: printf not available in Pancake - " +
  //       getSourceText(cursor)
  //       +
  //           " */",
  //       loc);
  // }

  // std::vector<ExprPtr> args;
  // for (size_t i = 1; i < children.size(); i++)
  //   args.push_back(buildExpr(children[i]));

  // return std::make_shared<CallExpr>(callee, args, loc);
  return {};
}

IRBuilder::BuiltExpression IRBuilder::buildIntLit(CXCursor cursor,
                                                  std::vector<StmtPtr> &stmts) {
  std::string text = getSourceText(cursor);
  BuiltExpression result;
  try {
    auto value = std::stoll(text, nullptr, 0);
    result.finalExpr = std::make_shared<IntLitExpr>(value, getLoc(cursor));
  } catch (...) {
    result.finalExpr = std::make_shared<RawExpr>(text, getLoc(cursor));
    result.preStmts.push_back(std::make_shared<CommentStmt>(
        "Failed to parse integer literal: " + text, getLoc(cursor)));
  }
  return result;
}

// increment/decrement (i++ -> i = i + 1)
IRBuilder::BuiltExpression
IRBuilder::tryBuildIncrDecr(CXCursor cursor, std::vector<StmtPtr> &stmts) {
  // auto info = extractUnaryOp(cursor);
  // if (info.op != "++" && info.op != "--")
  //   return nullptr;

  // auto children = getChildren(cursor);
  // if (children.empty())
  //   return nullptr;

  // std::string varName = getCursorSpelling(children[0]);
  // if (varName.empty())
  //   return nullptr;

  // auto loc = getLoc(cursor);
  // BinOp op = (info.op == "++") ? BinOp::Add : BinOp::Sub;
  // auto varRef = std::make_shared<VarRefExpr>(varName, loc);
  // auto one = std::make_shared<IntLitExpr>(1, loc);
  // auto expr = std::make_shared<BinaryExpr>(op, varRef, one, loc);

  // return std::make_shared<AssignStmt>(varName, expr, loc);
  return {};
}

// operator extraction via tokenization
// TODO replace with optional<string>
std::string IRBuilder::getCalleeName(CXCursor cursor) {
  assert(clang_getCursorKind(cursor) == CXCursor_CallExpr);

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
  auto clangLoc = clang_getCursorLocation(cursor);
  CXFile file;
  unsigned line, col;
  clang_getSpellingLocation(clangLoc, &file, &line, &col, nullptr);

  SourceLoc loc;
  loc.line = line;
  loc.col = col;
  if (file) {
    auto fileName = clang_getFileName(file);
    loc.file = clang_getCString(fileName);
    clang_disposeString(fileName);
  }
  return loc;
}

SourceLoc IRBuilder::getEndLoc(CXCursor cursor) {
  auto range = clang_getCursorExtent(cursor);
  auto endLoc = clang_getRangeEnd(range);
  CXFile file;
  unsigned line, col;
  clang_getSpellingLocation(endLoc, &file, &line, &col, nullptr);

  SourceLoc loc;
  loc.line = line;
  loc.col = col;
  if (file) {
    auto fileName = clang_getFileName(file);
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
