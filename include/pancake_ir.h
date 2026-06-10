#ifndef PANCAKE_IR_H
#define PANCAKE_IR_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pancake {
struct SourceLoc {
  std::string file;
  unsigned line = 0;
  unsigned col = 0;
};

enum class BinOp {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Lt,
  Gt,
  Le,
  Ge,
  Eq,
  Ne,
  And,
  Or,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  Shl,
  Shr,
};

enum class UnaryOp {
  Negate,
  Not,
  BitwiseNot,
};

struct Expr;
struct Stmt;
struct Block;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using BlockPtr = std::shared_ptr<Block>;

// expressions
enum class ExprKind {
  IntLit,
  VarRef,
  Binary,
  Unary,
  Call,
  Raw,
  StructLit,
  FieldAccess,
  MemoryLoad,
};

struct Expr {
  ExprKind kind;
  SourceLoc loc;
  virtual ~Expr() = default;

protected:
  Expr(ExprKind k, SourceLoc l = {}) : kind(k), loc(l) {}
};

struct IntLitExpr : Expr {
  int64_t value;
  IntLitExpr(int64_t v, SourceLoc l = {})
      : Expr(ExprKind::IntLit, l), value(v) {}
};

struct VarRefExpr : Expr {
  std::string name;
  VarRefExpr(std::string n, SourceLoc l = {})
      : Expr(ExprKind::VarRef, l), name(n) {}
};

struct BinaryExpr : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc = {})
      : Expr(ExprKind::Binary, loc), op(o), lhs(l), rhs(r) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(UnaryOp o, ExprPtr e, SourceLoc l = {})
      : Expr(ExprKind::Unary, l), op(o), operand(e) {}
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<ExprPtr> args;
  CallExpr(std::string c, std::vector<ExprPtr> a, SourceLoc l = {})
      : Expr(ExprKind::Call, l), callee(c), args(a) {}
};

// raw source text fallback
struct RawExpr : Expr {
  std::string text;
  RawExpr(std::string t, SourceLoc l = {}) : Expr(ExprKind::Raw, l), text(t) {}
};

struct StructLitExpr : Expr {
  std::vector<ExprPtr> fields;
  StructLitExpr(std::vector<ExprPtr> f, SourceLoc l = {})
      : Expr(ExprKind::StructLit, l), fields(f) {}
};

struct FieldAccessExpr : Expr {
  std::string varName;
  int fieldIndex;
  FieldAccessExpr(std::string v, int idx, SourceLoc l = {})
      : Expr(ExprKind::FieldAccess, l), varName(v), fieldIndex(idx) {}
};

struct MemoryLoadExpr : Expr {
  int shape;
  ExprPtr addrExpr;
  MemoryLoadExpr(int shape, ExprPtr addr, SourceLoc l = {})
      : Expr(ExprKind::MemoryLoad, l), shape(shape), addrExpr(addr) {}
};

// statements
enum class StmtKind {
  VarDecl,
  Assign,
  Return,
  If,
  While,
  ExprStmt,
  Comment,
  StructFieldAssign,
  Define,
  Break,
  Continue,
  MemoryStore,
  SharedMemoryStore, // TODO
  SharedMemoryLoad,  // TODO
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;
  virtual ~Stmt() = default;

protected:
  Stmt(StmtKind k, SourceLoc l = {}) : kind(k), loc(l) {}
};

struct Block {
  std::vector<StmtPtr> stmts;
  SourceLoc loc;
};

struct VarDeclStmt : Stmt {
  std::string name;
  std::optional<int> shape; // shape hint for call-initialized vars
  ExprPtr init;
  VarDeclStmt(std::string n, ExprPtr i = nullptr,
              std::optional<int> sh = std::nullopt, SourceLoc l = {})
      : Stmt(StmtKind::VarDecl, l), name(n), shape(sh), init(i) {}
};

struct AssignStmt : Stmt {
  std::string target;
  ExprPtr value;
  AssignStmt(std::string t, ExprPtr v, SourceLoc l = {})
      : Stmt(StmtKind::Assign, l), target(t), value(v) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;
  ReturnStmt(ExprPtr v = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::Return, l), value(v) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  BlockPtr thenBranch;
  BlockPtr elseBranch;
  IfStmt(ExprPtr c, BlockPtr t, BlockPtr e = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::If, l), condition(c), thenBranch(t), elseBranch(e) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  BlockPtr body;
  WhileStmt(ExprPtr c, BlockPtr b, SourceLoc l = {})
      : Stmt(StmtKind::While, l), condition(c), body(b) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  ExprStmt(ExprPtr e, SourceLoc l = {})
      : Stmt(StmtKind::ExprStmt, l), expr(e) {}
};

struct CommentStmt : Stmt {
  std::string text;
  CommentStmt(std::string t, SourceLoc l = {})
      : Stmt(StmtKind::Comment, l), text(t) {}
};

struct BreakStmt : Stmt {
  BreakStmt(SourceLoc l = {}) : Stmt(StmtKind::Break, l) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt(SourceLoc l = {}) : Stmt(StmtKind::Continue, l) {}
};

struct MemoryStoreStmt : Stmt {
  ExprPtr srcExpr;
  ExprPtr destExpr;
  MemoryStoreStmt(ExprPtr src, ExprPtr dest, SourceLoc l = {})
      : Stmt(StmtKind::MemoryStore, l), srcExpr(src), destExpr(dest) {}
};

struct DefineStmt : Stmt {
  std::string name;
  int64_t value;
  DefineStmt(std::string n, int64_t v, SourceLoc l = {})
      : Stmt(StmtKind::Define, l), name(n), value(v) {}
};

// struct field assign via whole-struct reassignment
struct StructFieldAssignStmt : Stmt {
  std::string structName;
  int fieldIndex;
  int fieldCount;
  ExprPtr value;
  StructFieldAssignStmt(std::string sn, int idx, int count, ExprPtr v,
                        SourceLoc l = {})
      : Stmt(StmtKind::StructFieldAssign, l), structName(sn), fieldIndex(idx),
        fieldCount(count), value(v) {}
};

// top-level
struct Param {
  std::string name;
  int shape = 1;
};

struct Function {
  std::string name;
  std::vector<Param> params;
  BlockPtr body;
  SourceLoc loc;
};

struct Program {
  std::vector<StmtPtr> globals;
  std::vector<StmtPtr> arrayInits; // prepended to main()
  std::vector<std::shared_ptr<Function>> functions;
};
} // namespace pancake
#endif
