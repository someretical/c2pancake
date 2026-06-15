#ifndef C2PANCAKE_IR_BUILDER_H
#define C2PANCAKE_IR_BUILDER_H

#include "pancake_ir.h"

#include <unordered_map>
#include <vector>

namespace pancake {
class IRBuilder {
public:
  ~IRBuilder();

  std::unique_ptr<Program> build(const std::string &filename);

  struct BuiltExpression {
    // the final expression to use
    ExprPtr finalExpr;
    // array indexing/deref ops may require tmp vars
    std::vector<StmtPtr> preStmts;
    std::vector<StmtPtr> postStmts;
  };

private:
  // struct type name -> field names
  struct StructInfo {
    std::vector<std::string> fieldNames;
  };
  std::unordered_map<std::string, StructInfo> structTypes;

  // var name -> struct type name
  std::unordered_map<std::string, std::string> structVars;

  // array name -> base slot and size
  struct ArrayInfo {
    int baseSlot;
    int size;
  };
  static constexpr int kInitialArraySlot = 100;
  std::unordered_map<std::string, ArrayInfo> globalArrays;
  int nextArraySlot = kInitialArraySlot;

  // temp var counters
  int switchVarCounter = 0;
  int ternaryVarCounter = 0;
  int arrayTmpCounter = 0;
  int derefTmpCounter = 0;
  int compoundAssignTmpCounter = 0;

  // top-level builders
  std::shared_ptr<Function> buildFunction(CXCursor cursor);
  BlockPtr buildBlock(CXCursor cursor);

  // statement builders
  std::vector<StmtPtr> buildStmt(CXCursor cursor);
  std::vector<StmtPtr> buildVarDecl(CXCursor cursor);
  std::vector<StmtPtr> buildForStmt(CXCursor cursor);
  std::vector<StmtPtr> buildStructDecl(CXCursor cursor);
  std::vector<StmtPtr> buildEnumDecl(CXCursor cursor);
  std::vector<StmtPtr> buildSwitchStmt(CXCursor cursor);

  // expression builders
  BuiltExpression buildExpr(CXCursor cursor);
  BuiltExpression buildBinaryExpr(CXCursor cursor);
  BuiltExpression buildCompoundAssignExpr(CXCursor cursor);
  BuiltExpression buildUnaryExpr(CXCursor cursor);
  BuiltExpression buildArraySubscriptExpr(CXCursor cursor);
  BuiltExpression buildCallExpr(CXCursor cursor);
  BuiltExpression buildIntLit(CXCursor cursor);
};
} // namespace pancake
#endif // C2PANCAKE_IR_BUILDER_H