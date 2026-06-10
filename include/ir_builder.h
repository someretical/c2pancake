#ifndef IR_BUILDER_H
#define IR_BUILDER_H

#include "pancake_ir.h"

#include <clang-c/Index.h>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pancake {
class IRBuilder {
public:
  ~IRBuilder();

  std::unique_ptr<Program> build(const std::string &filename);

  struct CursorHash {
    CXSourceLocation startLoc;
    CXSourceLocation endLoc;

    bool operator==(const CursorHash &other) const {
      return clang_equalLocations(startLoc, other.startLoc) &&
             clang_equalLocations(endLoc, other.endLoc);
    }
  };

private:
  CXTranslationUnit tu = nullptr;
  CXIndex index = nullptr;
  std::unordered_map<std::string, std::string> sourceCache;
  Program *currentProgram = nullptr;

  // struct type name -> field names
  struct StructInfo {
    std::vector<std::string> fieldNames;
  };
  std::unordered_map<std::string, StructInfo> structTypes;

  // var name -> struct type name
  std::unordered_map<std::string, std::string> structVars;

  // source line -> FFI name from @ffi annotations
  std::unordered_map<unsigned, std::string> ffiLineReplacements;

  // array name -> base slot and size
  struct ArrayInfo {
    int baseSlot;
    int size;
  };
  static constexpr int kInitialArraySlot = 100;
  std::unordered_map<std::string, ArrayInfo> globalArrays;
  int nextArraySlot = kInitialArraySlot;
  bool insideFunction = false;

  // RAII guard for insideFunction
  struct FunctionScope {
    bool &flag;
    FunctionScope(bool &f) : flag(f) { flag = true; }
    ~FunctionScope() { flag = false; }
    FunctionScope(const FunctionScope &) = delete;
    FunctionScope &operator=(const FunctionScope &) = delete;
  };

  // temp var counters
  int switchVarCounter = 0;
  int ternaryVarCounter = 0;
  int arrayTmpCounter = 0;
  int derefTmpCounter = 0;

  void scanFFIAnnotations(const std::string &filename);

  // helpers
  std::string resolveVarName(CXCursor cursor);
  static std::string arrayAddr(int baseSlot, const std::string &indexExpr);
  static std::string arrayAddr(int slot);
  BlockPtr buildBlockOrWrap(CXCursor cursor);
  static std::optional<BinOp> lookupBinOp(const std::string &opStr);
  static std::optional<UnaryOp> lookupUnaryOp(const std::string &opStr);
  int findStructFieldIndex(const std::string &varName,
                           const std::string &fieldName);
  int getStructFieldCount(const std::string &varName);
  static std::string stripStructPrefix(const std::string &name);

  // top-level builders
  std::shared_ptr<Function> buildFunction(CXCursor cursor);
  BlockPtr buildBlock(CXCursor cursor);

  // statement builders
  void buildStmt(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildVarDecl(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildForStmt(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildCompoundAssign(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildStructDecl(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildEnumDecl(CXCursor cursor, std::vector<StmtPtr> &stmts);
  void buildSwitchStmt(CXCursor cursor, std::vector<StmtPtr> &stmts);

  // expression builders
  ExprPtr buildExpr(CXCursor cursor);
  ExprPtr buildBinaryExpr(CXCursor cursor);
  ExprPtr buildUnaryExpr(CXCursor cursor);
  ExprPtr buildCallExpr(CXCursor cursor);
  ExprPtr buildIntLit(CXCursor cursor);
  ExprPtr tryExpandTernary(CXCursor cursor, std::vector<StmtPtr> &stmts);
  ExprPtr hoistArrayLoads(CXCursor cursor, std::vector<StmtPtr> &stmts);

  void pushExprStmt(std::vector<StmtPtr> &stmts, CXCursor cursor);

  // detect whether a cursor or any of its descendants may have side effects
  bool cursorHasSideEffects(CXCursor cursor);

  // for-loop helpers
  struct ForParts {
    CXCursor init, condition, update, body;
    bool hasInit = false, hasCond = false, hasUpdate = false;
  };
  ForParts classifyForChildren(CXCursor forStmt);
  void buildForUpdate(CXCursor cursor, std::vector<StmtPtr> &stmts);

  StmtPtr tryBuildIncrDecr(CXCursor cursor);

  // operator extraction
  std::string extractBinOp(CXCursor cursor);
  struct UnaryOpInfo {
    std::string op;
    bool isPrefix;
  };
  UnaryOpInfo extractUnaryOp(CXCursor cursor);
  std::string getCalleeName(CXCursor callExpr);

  // libclang helpers
  static std::vector<CXCursor> getChildren(CXCursor cursor);
  std::string getSourceText(CXCursor cursor);
  std::string getCursorSpelling(CXCursor cursor);
  static std::string getTypeSpelling(CXCursor cursor);
  static SourceLoc getLoc(CXCursor cursor);
  const std::string &getFileContent(CXFile file);
  std::string getSourceSlice(CXFile file, unsigned start, unsigned end);
  static bool getDescendant(CXCursor cursor, CXCursorKind kind);
};
} // namespace pancake
#endif
