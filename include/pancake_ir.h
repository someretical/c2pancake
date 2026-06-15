#ifndef C2PANCAKE_IR_H
#define C2PANCAKE_IR_H

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
  Negate, // pretty sure this is not supported in pancake
  Not,
  BitwiseNot, // this is also not supported
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
  StructLit,
  FieldAccess,
  MemoryLoad,
  Raw,
  ArrayIndex, // helper
  ArrayLoad,  // helper, complementary to ArrayStoreStmt
  BasePtr,
  TopPtr,
  BytesInWord,
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

struct BasePtrExpr : RawExpr {
  BasePtrExpr(SourceLoc l = {}) : RawExpr("@base", l) {}
};

struct TopPtrExpr : RawExpr {
  TopPtrExpr(SourceLoc l = {}) : RawExpr("@top", l) {}
};

struct BytesInWordExpr : RawExpr {
  BytesInWordExpr(SourceLoc l = {}) : RawExpr("@biw", l) {}
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

struct ArrayIndexExpr : Expr {
  ExprPtr idx;
  ExprPtr step;
  ArrayIndexExpr(ExprPtr i, ExprPtr s, SourceLoc l = {})
      : Expr(ExprKind::ArrayIndex, l), idx(i), step(s) {}
};

struct ArrayLoadExpr : Expr {
  int baseSlot;
  ExprPtr indexExpr; // this should be a ArrayIndexExpr!!!
  ArrayLoadExpr(int bs, ExprPtr idx, SourceLoc l = {})
      : Expr(ExprKind::ArrayLoad, l), baseSlot(bs), indexExpr(idx) {}
};

// statements
enum class StmtKind {
  Block,
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
  ArrayStore,        // helper for array element assignment
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

struct BlockStmt : Stmt {
  BlockPtr block;
  SourceLoc loc;
  BlockStmt(BlockPtr b, SourceLoc l = {})
      : Stmt(StmtKind::Block, l), block(b) {}
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
  ExprPtr target;
  ExprPtr value;
  AssignStmt(ExprPtr t, ExprPtr v, SourceLoc l = {})
      : Stmt(StmtKind::Assign, l), target(t), value(v) {}
  AssignStmt(std::string t, ExprPtr v, SourceLoc l = {})
      : Stmt(StmtKind::Assign, l), target(std::make_shared<VarRefExpr>(t, l)),
        value(v) {}
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

struct ArrayStoreStmt : Stmt {
  int baseSlot;
  ExprPtr indexExpr; // this should be a ArrayIndexExpr!!!
  ExprPtr valueExpr;
  ArrayStoreStmt(int bs, ExprPtr idx, ExprPtr val, SourceLoc l = {})
      : Stmt(StmtKind::ArrayStore, l), baseSlot(bs), indexExpr(idx),
        valueExpr(val) {}
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
  size_t shape = 1;
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
#endif // C2PANCAKE_IR_H
