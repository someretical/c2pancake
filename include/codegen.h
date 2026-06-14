#ifndef C2PANCAKE_CODEGEN_H
#define C2PANCAKE_CODEGEN_H

#include "pancake_ir.h"

#include <sstream>
#include <string>

namespace pancake {
class CodeGen {
public:
  std::string generate(const Program &program);

private:
  std::ostringstream out;
  int indentLevel = 0;

  void emit(const Function &func);
  void emitBlock(const Block &block, bool appendSemicolon);
  void emitStmt(const Stmt &stmt);
  void emitExpr(const Expr &expr, int parentPrec = -1);

  std::string indentStr() const;
  void increaseIndent() { indentLevel++; }
  void decreaseIndent() { indentLevel--; }

  static int precedence(BinOp op);
  static const char *opString(BinOp op);
  static const char *opString(UnaryOp op);
  static bool needsSemicolon(const Stmt &stmt);
};
} // namespace pancake
#endif // C2PANCAKE_CODEGEN_H
