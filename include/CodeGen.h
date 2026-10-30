#ifndef C2PANCAKE_CODEGEN_H
#define C2PANCAKE_CODEGEN_H

#include "pancake_ir.h"

#include <sstream>
#include <string>

namespace pancake {
class CodeGen {
public:
  auto Generate(const Program &program) -> std::string;
  auto generate(const Program &program) -> std::string { return Generate(program); }

private:
  std::ostringstream out;
  int indent_level = 0;

  void Emit(const Function &function);
  void EmitBlock(const Block &block);
  void EmitStmt(const Stmt &stmt);
  void EmitExpr(const Expr &expr, int parent_precedence = -1);
  static auto Precedence(BinOp op) -> int;
  static auto OpString(BinOp op) -> const char *;
  static auto OpString(UnaryOp op) -> const char *;
  static auto LoadStoreSuffix(MemoryWidth width) -> const char *;
  static auto NeedsSemicolon(const Stmt &stmt) -> bool;
  auto Indent() const -> std::string;
};
} // namespace pancake

#endif // C2PANCAKE_CODEGEN_H
