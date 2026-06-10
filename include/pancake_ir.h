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

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using BlockPtr = std::unique_ptr<Block>;

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
};

struct Expr {
  ExprKind kind;
  SourceLoc loc;
  virtual ~Expr() = default;

protected:
  Expr(ExprKind k, SourceLoc l = {}) : kind(k), loc(std::move(l)) {}
};

struct IntLitExpr : Expr {
  int64_t value;
  IntLitExpr(int64_t v, SourceLoc l = {})
      : Expr(ExprKind::IntLit, std::move(l)), value(v) {}
};

struct VarRefExpr : Expr {
  std::string name;
  VarRefExpr(std::string n, SourceLoc l = {})
      : Expr(ExprKind::VarRef, std::move(l)), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc = {})
      : Expr(ExprKind::Binary, std::move(loc)), op(o), lhs(std::move(l)),
        rhs(std::move(r)) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(UnaryOp o, ExprPtr e, SourceLoc l = {})
      : Expr(ExprKind::Unary, std::move(l)), op(o), operand(std::move(e)) {}
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<ExprPtr> args;
  CallExpr(std::string c, std::vector<ExprPtr> a, SourceLoc l = {})
      : Expr(ExprKind::Call, std::move(l)), callee(std::move(c)),
        args(std::move(a)) {}
};

// raw source text fallback
struct RawExpr : Expr {
  std::string text;
  RawExpr(std::string t, SourceLoc l = {})
      : Expr(ExprKind::Raw, std::move(l)), text(std::move(t)) {}
};

struct StructLitExpr : Expr {
  std::vector<ExprPtr> fields;
  StructLitExpr(std::vector<ExprPtr> f, SourceLoc l = {})
      : Expr(ExprKind::StructLit, std::move(l)), fields(std::move(f)) {}
};

struct FieldAccessExpr : Expr {
  std::string varName;
  int fieldIndex;
  FieldAccessExpr(std::string v, int idx, SourceLoc l = {})
      : Expr(ExprKind::FieldAccess, std::move(l)), varName(std::move(v)),
        fieldIndex(idx) {}
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
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;
  virtual ~Stmt() = default;

protected:
  Stmt(StmtKind k, SourceLoc l = {}) : kind(k), loc(std::move(l)) {}
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
      : Stmt(StmtKind::VarDecl, std::move(l)), name(std::move(n)), shape(sh),
        init(std::move(i)) {}
};

struct AssignStmt : Stmt {
  std::string target;
  ExprPtr value;
  AssignStmt(std::string t, ExprPtr v, SourceLoc l = {})
      : Stmt(StmtKind::Assign, std::move(l)), target(std::move(t)),
        value(std::move(v)) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;
  ReturnStmt(ExprPtr v = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::Return, std::move(l)), value(std::move(v)) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  BlockPtr thenBranch;
  BlockPtr elseBranch;
  IfStmt(ExprPtr c, BlockPtr t, BlockPtr e = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::If, std::move(l)), condition(std::move(c)),
        thenBranch(std::move(t)), elseBranch(std::move(e)) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  BlockPtr body;
  WhileStmt(ExprPtr c, BlockPtr b, SourceLoc l = {})
      : Stmt(StmtKind::While, std::move(l)), condition(std::move(c)),
        body(std::move(b)) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  ExprStmt(ExprPtr e, SourceLoc l = {})
      : Stmt(StmtKind::ExprStmt, std::move(l)), expr(std::move(e)) {}
};

struct CommentStmt : Stmt {
  std::string text;
  CommentStmt(std::string t, SourceLoc l = {})
      : Stmt(StmtKind::Comment, std::move(l)), text(std::move(t)) {}
};

struct BreakStmt : Stmt {
  BreakStmt(SourceLoc l = {}) : Stmt(StmtKind::Break, std::move(l)) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt(SourceLoc l = {}) : Stmt(StmtKind::Continue, std::move(l)) {}
};

struct DefineStmt : Stmt {
  std::string name;
  int64_t value;
  DefineStmt(std::string n, int64_t v, SourceLoc l = {})
      : Stmt(StmtKind::Define, std::move(l)), name(std::move(n)), value(v) {}
};

// struct field assign via whole-struct reassignment
struct StructFieldAssignStmt : Stmt {
  std::string structName;
  int fieldIndex;
  int fieldCount;
  ExprPtr value;
  StructFieldAssignStmt(std::string sn, int idx, int count, ExprPtr v,
                        SourceLoc l = {})
      : Stmt(StmtKind::StructFieldAssign, std::move(l)),
        structName(std::move(sn)), fieldIndex(idx), fieldCount(count),
        value(std::move(v)) {}
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
  std::vector<std::unique_ptr<Function>> functions;
};
} // namespace pancake
#endif
