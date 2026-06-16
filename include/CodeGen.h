#ifndef C2PANCAKE_CODEGEN_H
#define C2PANCAKE_CODEGEN_H

#include "PancakeIR.h"

#include <cstddef>
#include <sstream>
#include <string>

namespace pancake {
class CodeGen {
public:
  auto generate(const Program &program) -> std::string;

private:
  std::ostringstream out;
  size_t indentLevel = 0;

  void emit(const Function &func);
  void emitBlock(const Block &block, bool appendSemicolon);
  void emitStmt(const Stmt &stmt);
  void emitExpr(const Expr &expr, int parentPrec = -1);

  auto indentStr() const -> std::string;
  void increaseIndent() { indentLevel++; }
  void decreaseIndent() { indentLevel--; }

  static auto precedence(BinOp op) -> int;
  static auto opString(BinOp op) -> const char *;
  static auto opString(UnaryOp op) -> const char *;
  static auto needsSemicolon(const Stmt &stmt) -> bool;
};
} // namespace pancake

#endif // C2PANCAKE_CODEGEN_H
