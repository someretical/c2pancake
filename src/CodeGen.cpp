#include "CodeGen.h"

namespace pancake {
auto CodeGen::Precedence(BinOp op) -> int {
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

auto CodeGen::OpString(BinOp op) -> const char * {
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

auto CodeGen::OpString(UnaryOp op) -> const char * {
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

auto CodeGen::LoadStoreSuffix(MemoryWidth width) -> const char * {
  switch (width) {
  case MemoryWidth::Word:
    return "w";
  case MemoryWidth::Byte:
    return "8";
  case MemoryWidth::Word32:
    return "32";
  }
  return "w";
}

auto CodeGen::NeedsSemicolon(const Stmt &stmt) -> bool {
  switch (stmt.kind) {
  case StmtKind::Block:
  case StmtKind::If:
  case StmtKind::While:
  case StmtKind::Comment:
  case StmtKind::Define:
    return false;
  default:
    return true;
  }
}

auto CodeGen::Indent() const -> std::string { return std::string(static_cast<size_t>(indent_level) * 2, ' '); }

auto CodeGen::Generate(const Program &program) -> std::string {
  out.str("");
  out.clear();
  indent_level = 0;
  for (const auto &stmt : program.globals) {
    EmitStmt(*stmt);
    if (NeedsSemicolon(*stmt))
      out << ";";
    out << "\n";
  }
  for (const auto &function : program.functions) {
    out << "\n";
    Emit(*function);
    out << "\n";
  }
  return out.str();
}

void CodeGen::Emit(const Function &function) {
  if (function.isInline)
    out << "inline ";
  out << "fun 1 " << function.name << "(";
  for (size_t i = 0; i < function.params.size(); ++i) {
    if (i != 0)
      out << ", ";
    out << function.params[i].shape << " " << function.params[i].name;
  }
  out << ") ";
  EmitBlock(*function.body);
}

void CodeGen::EmitBlock(const Block &block) {
  out << "{\n";
  ++indent_level;
  for (const auto &stmt : block.stmts) {
    out << Indent();
    EmitStmt(*stmt);
    if (NeedsSemicolon(*stmt))
      out << ";";
    out << "\n";
  }
  --indent_level;
  out << Indent() << "}";
}

void CodeGen::EmitStmt(const Stmt &stmt) {
  switch (stmt.kind) {
  case StmtKind::Comment:
    out << "/* " << static_cast<const CommentStmt &>(stmt).text << " */";
    break;
  case StmtKind::Block:
    EmitBlock(*static_cast<const BlockStmt &>(stmt).block);
    break;
  case StmtKind::VarDecl: {
    const auto &s = static_cast<const VarDeclStmt &>(stmt);
    out << "var " << s.shape.value_or(1) << " " << s.name;
    if (s.init) {
      out << " = ";
      EmitExpr(*s.init);
    }
    break;
  }
  case StmtKind::Assign: {
    const auto &s = static_cast<const AssignStmt &>(stmt);
    EmitExpr(*s.target);
    out << " = ";
    EmitExpr(*s.value);
    break;
  }
  case StmtKind::Return: {
    const auto &s = static_cast<const ReturnStmt &>(stmt);
    out << "return";
    if (s.value) {
      out << " ";
      EmitExpr(*s.value);
    } else
      out << " 0";
    break;
  }
  case StmtKind::If: {
    const auto &s = static_cast<const IfStmt &>(stmt);
    out << "if (";
    EmitExpr(*s.condition);
    out << ") ";
    EmitBlock(*s.thenBranch);
    if (s.elseBranch) {
      out << " else ";
      EmitBlock(*s.elseBranch);
    }
    break;
  }
  case StmtKind::While: {
    const auto &s = static_cast<const WhileStmt &>(stmt);
    out << "while (";
    EmitExpr(*s.condition);
    out << ") ";
    EmitBlock(*s.body);
    break;
  }
  case StmtKind::ExprStmt:
    EmitExpr(*static_cast<const ExprStmt &>(stmt).expr);
    break;
  case StmtKind::Break:
    out << "break";
    break;
  case StmtKind::Continue:
    out << "continue";
    break;
  case StmtKind::Define: {
    const auto &s = static_cast<const DefineStmt &>(stmt);
    out << "#define " << s.name << " " << s.value;
    break;
  }
  case StmtKind::MemoryStore: {
    const auto &s = static_cast<const MemoryStoreStmt &>(stmt);
    out << "st";
    if (s.width != MemoryWidth::Word)
      out << LoadStoreSuffix(s.width);
    out << " ";
    EmitExpr(*s.destExpr);
    out << ", ";
    EmitExpr(*s.srcExpr);
    break;
  }
  case StmtKind::SharedMemoryStore: {
    const auto &s = static_cast<const SharedMemoryStoreStmt &>(stmt);
    out << "!st" << LoadStoreSuffix(s.width) << " ";
    EmitExpr(*s.destExpr);
    out << ", ";
    EmitExpr(*s.srcExpr);
    break;
  }
  case StmtKind::SharedMemoryLoad: {
    const auto &s = static_cast<const SharedMemoryLoadStmt &>(stmt);
    out << "!ld" << LoadStoreSuffix(s.width) << " " << s.target << ", ";
    EmitExpr(*s.addrExpr);
    break;
  }
  case StmtKind::StructFieldAssign: {
    const auto &s = static_cast<const StructFieldAssignStmt &>(stmt);
    out << s.structName << " = <";
    for (int i = 0; i < s.fieldCount; ++i) {
      if (i != 0)
        out << ", ";
      if (i == s.fieldIndex)
        EmitExpr(*s.value);
      else
        out << s.structName << "." << i;
    }
    out << ">";
    break;
  }
  }
}

void CodeGen::EmitExpr(const Expr &expr, int parent_precedence) {
  switch (expr.kind) {
  case ExprKind::IntLit:
    out << static_cast<const IntLitExpr &>(expr).value;
    break;
  case ExprKind::VarRef:
    out << static_cast<const VarRefExpr &>(expr).name;
    break;
  case ExprKind::Raw:
  case ExprKind::BasePtr:
  case ExprKind::TopPtr:
  case ExprKind::BytesInWord:
    out << static_cast<const RawExpr &>(expr).text;
    break;
  case ExprKind::Binary: {
    const auto &e = static_cast<const BinaryExpr &>(expr);
    const int precedence = Precedence(e.op);
    const bool parens = parent_precedence > precedence;
    if (parens)
      out << "(";
    EmitExpr(*e.lhs, precedence);
    out << " " << OpString(e.op) << " ";
    EmitExpr(*e.rhs, precedence + 1);
    if (parens)
      out << ")";
    break;
  }
  case ExprKind::Unary: {
    const auto &e = static_cast<const UnaryExpr &>(expr);
    out << OpString(e.op);
    EmitExpr(*e.operand, 100);
    break;
  }
  case ExprKind::Call: {
    const auto &e = static_cast<const CallExpr &>(expr);
    out << e.callee << "(";
    for (size_t i = 0; i < e.args.size(); ++i) {
      if (i != 0)
        out << ", ";
      EmitExpr(*e.args[i]);
    }
    out << ")";
    break;
  }
  case ExprKind::StructLit: {
    const auto &e = static_cast<const StructLitExpr &>(expr);
    out << "<";
    for (size_t i = 0; i < e.fields.size(); ++i) {
      if (i != 0)
        out << ", ";
      EmitExpr(*e.fields[i]);
    }
    out << ">";
    break;
  }
  case ExprKind::FieldAccess: {
    const auto &e = static_cast<const FieldAccessExpr &>(expr);
    out << e.varName << "." << e.fieldIndex;
    break;
  }
  case ExprKind::MemoryLoad: {
    const auto &e = static_cast<const MemoryLoadExpr &>(expr);
    if (e.width == MemoryWidth::Word)
      out << "(lds 1 (";
    else
      out << "(ld" << LoadStoreSuffix(e.width) << " (";
    EmitExpr(*e.addrExpr);
    out << "))";
    break;
  }
  }
}
} // namespace pancake
