#include "codegen.h"

#include <cassert>

namespace pancake {

int CodeGen::precedence(BinOp op) {
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

const char *CodeGen::opString(BinOp op) {
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
  return "?";
}

const char *CodeGen::opString(UnaryOp op) {
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

bool CodeGen::needsSemicolon(const Stmt &stmt) {
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

std::string CodeGen::indentStr() const {
  return std::string(indentLevel * 4, ' ');
}

std::string CodeGen::generate(const Program &program) {
  out.str("");
  out.clear();
  indentLevel = 0;

  out << "// Generated Pancake code from C\n";

  for (auto &stmt : program.globals) {
    emitStmt(*stmt);
    if (needsSemicolon(*stmt))
      out << ";";
    out << "\n";
  }

  for (auto &func : program.functions) {
    out << "\n";
    emit(*func);
    out << "\n";
  }

  out << "\n";

  return out.str();
}

void CodeGen::emit(const Function &func) {
  out << "fun " << func.name << "(";

  for (size_t i = 0; i < func.params.size(); i++) {
    if (i > 0)
      out << ", ";
    out << func.params[i].shape << " " << func.params[i].name;
  }
  out << ") ";

  if (func.body) {
    emitBlock(*func.body);
  } else {
    out << "{\n}\n";
  }
}

void CodeGen::emitBlock(const Block &block) {
  out << "{\n";
  increaseIndent();

  for (auto &stmt : block.stmts) {
    out << indentStr();
    emitStmt(*stmt);
    if (needsSemicolon(*stmt))
      out << ";";
    out << "\n";
  }

  decreaseIndent();
  out << indentStr() << "}";
}

void CodeGen::emitStmt(const Stmt &stmt) {
  switch (stmt.kind) {
  case StmtKind::VarDecl: {
    auto &s = static_cast<const VarDeclStmt &>(stmt);
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
    auto &s = static_cast<const AssignStmt &>(stmt);
    out << s.target << " = ";
    emitExpr(*s.value);
    break;
  }
  case StmtKind::Return: {
    auto &s = static_cast<const ReturnStmt &>(stmt);
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
    auto &s = static_cast<const IfStmt &>(stmt);
    out << "if (";
    emitExpr(*s.condition);
    out << ") ";
    emitBlock(*s.thenBranch);
    if (s.elseBranch) {
      out << " else ";
      emitBlock(*s.elseBranch);
    }
    break;
  }
  case StmtKind::While: {
    auto &s = static_cast<const WhileStmt &>(stmt);
    out << "while (";
    emitExpr(*s.condition);
    out << ") ";
    emitBlock(*s.body);
    break;
  }
  case StmtKind::ExprStmt: {
    auto &s = static_cast<const ExprStmt &>(stmt);
    emitExpr(*s.expr);
    break;
  }
  case StmtKind::Comment: {
    auto &s = static_cast<const CommentStmt &>(stmt);
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
    auto &s = static_cast<const DefineStmt &>(stmt);
    out << "#define " << s.name << " " << s.value;
    break;
  }
  case StmtKind::StructFieldAssign: {
    // rebuild struct tuple with updated field
    auto &s = static_cast<const StructFieldAssignStmt &>(stmt);
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
    auto &s = static_cast<const MemoryStoreStmt &>(stmt);
    out << "st ";
    emitExpr(*s.destExpr);
    out << ", ";
    emitExpr(*s.srcExpr);
    break;
  }
  case StmtKind::SharedMemoryStore: {
    // TODO
    break;
  }
  case StmtKind::SharedMemoryLoad: {
    // TODO
    break;
  }
  }
}

void CodeGen::emitExpr(const Expr &expr, int parentPrec) {
  switch (expr.kind) {
  case ExprKind::IntLit: {
    auto &e = static_cast<const IntLitExpr &>(expr);
    out << e.value;
    break;
  }
  case ExprKind::VarRef: {
    auto &e = static_cast<const VarRefExpr &>(expr);
    out << e.name;
    break;
  }
  case ExprKind::Binary: {
    auto &e = static_cast<const BinaryExpr &>(expr);
    int myPrec = precedence(e.op);
    bool needParens = (parentPrec > myPrec);

    if (needParens)
      out << "(";
    emitExpr(*e.lhs, myPrec);
    out << " " << opString(e.op) << " ";
    // +1 for left-associativity
    emitExpr(*e.rhs, myPrec + 1);
    if (needParens)
      out << ")";
    break;
  }
  case ExprKind::Unary: {
    auto &e = static_cast<const UnaryExpr &>(expr);
    out << opString(e.op);
    emitExpr(*e.operand, 100); // force parens on complex operands
    break;
  }
  case ExprKind::Call: {
    auto &e = static_cast<const CallExpr &>(expr);
    out << e.callee << "(";
    for (size_t i = 0; i < e.args.size(); i++) {
      if (i > 0)
        out << ", ";
      emitExpr(*e.args[i]);
    }
    out << ")";
    break;
  }
  case ExprKind::Raw: {
    auto &e = static_cast<const RawExpr &>(expr);
    out << e.text;
    break;
  }
  case ExprKind::StructLit: {
    auto &e = static_cast<const StructLitExpr &>(expr);
    out << "<";
    for (size_t i = 0; i < e.fields.size(); i++) {
      if (i > 0)
        out << ", ";
      emitExpr(*e.fields[i]);
    }
    out << ">";
    break;
  }
  case ExprKind::FieldAccess: {
    auto &e = static_cast<const FieldAccessExpr &>(expr);
    out << e.varName << "." << e.fieldIndex;
    break;
  }
  case ExprKind::MemoryLoad: {
    auto &e = static_cast<const MemoryLoadExpr &>(expr);
    // TODO only lds 1 <addr> actually compiles
    out << "lds " << e.shape << " ";
    emitExpr(*e.addrExpr);
    break;
  }
  }
}

} // namespace pancake
