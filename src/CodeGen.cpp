#include "CodeGen.h"
#include "IRBuilder.h"
#include "PancakeIR.h"

#include <cassert>
#include <format>
#include <ranges>
#include <string>

namespace pancake {
auto CodeGen::precedence(BinOp op) -> int {
  switch (op) {
  case BinOp::Or:
    return 1;
  case BinOp::And:
    return 2;
  case BinOp::BitwiseOr:
    return 3;
  case BinOp::BitwiseXor:
    return 4;
  case BinOp::BitwiseAnd:
    return 5;
  case BinOp::Eq:
  case BinOp::Ne:
    return 6;
  case BinOp::Lt:
  case BinOp::Gt:
  case BinOp::Le:
  case BinOp::Ge:
    return 7;
  case BinOp::Shl:
  case BinOp::Shr:
    return 8;
  case BinOp::Add:
  case BinOp::Sub:
    return 9;
  case BinOp::Mul:
  case BinOp::Div:
  case BinOp::Mod:
    return 10;
  }
  return 0;
}

auto CodeGen::opString(BinOp op) -> const char * {
  switch (op) {
  case BinOp::Add:
    return "+";
  case BinOp::Sub:
    return "-";
  case BinOp::Mul:
    return "*";
  case BinOp::Div:
    return "/";
  case BinOp::Mod:
    return "%";
  case BinOp::Lt:
    return "<";
  case BinOp::Gt:
    return ">";
  case BinOp::Le:
    return "<=";
  case BinOp::Ge:
    return ">=";
  case BinOp::Eq:
    return "==";
  case BinOp::Ne:
    return "!=";
  case BinOp::And:
    return "&&";
  case BinOp::Or:
    return "||";
  case BinOp::BitwiseAnd:
    return "&";
  case BinOp::BitwiseOr:
    return "|";
  case BinOp::BitwiseXor:
    return "^";
  case BinOp::Shl:
    return "<<";
  case BinOp::Shr:
    return ">>";
  }
  return "<?>";
}

auto CodeGen::opString(UnaryOp op) -> const char * {
  switch (op) {
  case UnaryOp::Negate:
    return "-";
  case UnaryOp::Not:
    return "!";
  case UnaryOp::BitwiseNot:
    return "~";
  }
  return "?";
}

auto CodeGen::needsSemicolon(const Stmt &stmt) -> bool {
  switch (stmt.kind) {
  case StmtKind::If:
  case StmtKind::While:
  case StmtKind::Comment:
  case StmtKind::Define:
    return false;
  default:
    return true;
  }
}

auto CodeGen::indentStr() const -> std::string {
  auto str = std::string(indentLevel * 4, ' ');
  return str;
}

auto CodeGen::generate(const IRBuilder &builder) -> std::string {
  out.str("");
  out.clear();
  indentLevel = 0;

  out << "// Generated Pancake code from C\n";

  for (const auto &stmt : builder.getGlobals()) {
    emitStmt(*stmt.get());
    if (needsSemicolon(*stmt.get()))
      out << ";";
    out << "\n";
  }

  for (const auto &func : builder.getFunctions()) {
    out << "\n";
    emit(func.get());
    out << "\n";
  }

  out << "\n";

  return out.str();
}

// NOLINTNEXTLINE(misc-no-recursion)
void CodeGen::emit(const Function &func) {
  out << "fun " << func.name << "(";

  for (const auto &[i, p] : std::views::enumerate(func.params)) {
    if (i > 0)
      out << ", ";
    out << p.shape << " " << p.name;
  }

  out << ") ";

  if (func.body) {
    emitBlock(*func.body, false);
  } else {
    out << "{\n}\n";
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void CodeGen::emitBlock(const Block &block, bool /*appendSemicolon*/) {
  out << "{\n";
  increaseIndent();

  for (const auto &stmt : block.stmts) {
    out << indentStr();
    emitStmt(*stmt);
    if (needsSemicolon(*stmt))
      out << ";";
    out << "\n";
  }

  decreaseIndent();
  out << indentStr() << "}";
}

// NOLINTNEXTLINE(misc-no-recursion)
void CodeGen::emitStmt(const Stmt &stmt) {
  switch (stmt.kind) {
  case StmtKind::Block: {
    const auto &s = dynamic_cast<const BlockStmt &>(stmt);
    emitBlock(*s.block, true);
    break;
  }
  case StmtKind::VarDecl: {
    const auto &s = dynamic_cast<const VarDeclStmt &>(stmt);
    out << "var ";
    if (s.shape.has_value())
      out << s.shape.value() << " ";
    out << s.name;
    if (s.init) {
      out << " = ";
      emitExpr(*s.init);
    }
    break;
  }
  case StmtKind::Assign: {
    const auto &s = dynamic_cast<const AssignStmt &>(stmt);
    emitExpr(*s.target);
    out << " = ";
    emitExpr(*s.value);
    break;
  }
  case StmtKind::Return: {
    const auto &s = dynamic_cast<const ReturnStmt &>(stmt);
    out << "return";
    if (s.value) {
      out << " ";
      emitExpr(*s.value);
    } else {
      out << " 0";
    }
    break;
  }
  case StmtKind::If: {
    const auto &s = dynamic_cast<const IfStmt &>(stmt);
    out << "if (";
    emitExpr(*s.condition);
    out << ") ";
    emitBlock(*s.thenBranch, false);
    if (s.elseBranch) {
      out << " else ";
      emitBlock(*s.elseBranch, false);
    }
    break;
  }
  case StmtKind::While: {
    const auto &s = dynamic_cast<const WhileStmt &>(stmt);
    out << "while (";
    emitExpr(*s.condition);
    out << ") ";
    emitBlock(*s.body, false);
    break;
  }
  case StmtKind::ExprStmt: {
    const auto &s = dynamic_cast<const ExprStmt &>(stmt);
    emitExpr(*s.expr);
    break;
  }
  case StmtKind::Comment: {
    const auto &s = dynamic_cast<const CommentStmt &>(stmt);
    out << "// " << s.text;
    break;
  }
  case StmtKind::Break: {
    out << "break";
    break;
  }
  case StmtKind::Continue: {
    out << "continue";
    break;
  }
  case StmtKind::Define: {
    const auto &s = dynamic_cast<const DefineStmt &>(stmt);
    out << "#define " << s.name << " " << s.value;
    break;
  }
  case StmtKind::StructFieldAssign: {
    // rebuild struct tuple with updated field
    const auto &s = dynamic_cast<const StructFieldAssignStmt &>(stmt);
    out << s.structName << " = <";
    for (int i = 0; i < s.fieldCount; i++) {
      if (i > 0)
        out << ", ";
      if (i == s.fieldIndex) {
        emitExpr(*s.value);
      } else {
        out << s.structName << "." << i;
      }
    }
    out << ">";
    break;
  }
  case StmtKind::MemoryStore: {
    const auto &s = dynamic_cast<const MemoryStoreStmt &>(stmt);
    out << "st ";
    emitExpr(*s.destExpr);
    out << ", ";
    emitExpr(*s.srcExpr);
    break;
  }
  case StmtKind::ArrayStore: {
    // st @base + (baseSlot + indexExpr), valueExpr
    // note that ArrayIndexExpr calculates the step size for us
    const auto &s = dynamic_cast<const ArrayStoreStmt &>(stmt);
    out << std::format("st @base + ({} + (", s.baseSlot);
    emitExpr(*s.indexExpr);
    out << "), ";
    emitExpr(*s.valueExpr);
    break;
  }
  case StmtKind::SharedMemoryStore:
  case StmtKind::SharedMemoryLoad: {
    // TODO
    break;
  }
  }
}

// NOLINTNEXTLINE(misc-no-recursion)
void CodeGen::emitExpr(const Expr &expr, int parentPrec) {
  switch (expr.kind) {
  case ExprKind::IntLit: {
    const auto &e = dynamic_cast<const IntLitExpr &>(expr);
    out << e.value;
    break;
  }
  case ExprKind::VarRef: {
    const auto &e = dynamic_cast<const DeclRefExpr &>(expr);
    out << e.name;
    break;
  }
  case ExprKind::Binary: {
    const auto &e = dynamic_cast<const BinaryExpr &>(expr);
    int const my_prec = precedence(e.op);
    bool const need_parens = (parentPrec > my_prec);

    if (need_parens)
      out << "(";
    emitExpr(*e.lhs, my_prec);
    out << " " << opString(e.op) << " ";
    // +1 for left-associativity
    emitExpr(*e.rhs, my_prec + 1);
    if (need_parens)
      out << ")";
    break;
  }
  case ExprKind::Unary: {
    const auto &e = dynamic_cast<const UnaryExpr &>(expr);
    out << opString(e.op);
    emitExpr(*e.operand, 100); // force parens on complex operands
    break;
  }
  case ExprKind::Call: {
    const auto &e = dynamic_cast<const CallExpr &>(expr);
    out << e.callee << "(";

    for (const auto &[i, arg] : std::views::enumerate(e.args)) {
      if (i > 0)
        out << ", ";
      emitExpr(*arg);
    }

    out << ")";
    break;
  }
  case ExprKind::BasePtr:
  case ExprKind::TopPtr:
  case ExprKind::BytesInWord:
  case ExprKind::Raw: {
    const auto &e = dynamic_cast<const RawExpr &>(expr);
    out << e.text;
    break;
  }
  case ExprKind::StructLit: {
    const auto &e = dynamic_cast<const StructLitExpr &>(expr);
    out << "<";
    for (const auto &[i, field] : std::views::enumerate(e.fields)) {
      if (i > 0)
        out << ", ";
      emitExpr(*field);
    }
    out << ">";
    break;
  }
  case ExprKind::FieldAccess: {
    const auto &e = dynamic_cast<const FieldAccessExpr &>(expr);
    out << e.varName << "." << e.fieldIndex;
    break;
  }
  case ExprKind::MemoryLoad: {
    const auto &e = dynamic_cast<const MemoryLoadExpr &>(expr);
    // TODO only lds 1 <addr> actually compiles
    out << std::format("(lds {} (", e.shape);
    emitExpr(*e.addrExpr);
    out << "))";
    break;
  }
  case ExprKind::ArrayIndex: {
    const auto &e = dynamic_cast<const ArrayIndexExpr &>(expr);
    // this is just a helper for calculating the step size for array accesses
    // it should never appear in the final output
    bool const need_parens = parentPrec > precedence(BinOp::Mul);
    if (need_parens)
      out << "(";
    emitExpr(*e.idx);
    out << " * ";
    emitExpr(*e.step);
    if (need_parens)
      out << ")";
    break;
  }
  case ExprKind::ArrayLoad: {
    const auto &e = dynamic_cast<const ArrayLoadExpr &>(expr);
    // this is a helper for array element access, it should be emitted as
    // lds 1 @base + (baseSlot + indexExpr)
    // TODO figure out how to support different load sizes
    out << std::format("(lds 1 (@base + {} + (", e.baseSlot);
    emitExpr(*e.indexExpr);
    out << ")))";
    break;
  }
  }
}

} // namespace pancake
