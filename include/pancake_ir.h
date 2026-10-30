#ifndef C2PANCAKE_IR_H
#define C2PANCAKE_IR_H

#include <cstddef>
#include <cstdint>
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

enum class MemoryWidth {
  Word,
  Byte,
  Word32,
};

struct Expr;
struct Stmt;
struct Block;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using BlockPtr = std::shared_ptr<Block>;

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
  BasePtr,
  TopPtr,
  BytesInWord,
};

struct Expr {
  ExprKind kind;
  SourceLoc loc;

  virtual ~Expr() = default;

protected:
  explicit Expr(ExprKind kind, SourceLoc loc = {}) : kind(kind), loc(std::move(loc)) {}
};

struct IntLitExpr final : Expr {
  std::int64_t value;

  explicit IntLitExpr(std::int64_t value, SourceLoc loc = {}) : Expr(ExprKind::IntLit, std::move(loc)), value(value) {}
};

struct VarRefExpr final : Expr {
  std::string name;

  explicit VarRefExpr(std::string name, SourceLoc loc = {})
      : Expr(ExprKind::VarRef, std::move(loc)), name(std::move(name)) {}
};

struct BinaryExpr final : Expr {
  BinOp op;
  ExprPtr lhs;
  ExprPtr rhs;

  BinaryExpr(BinOp op, ExprPtr lhs, ExprPtr rhs, SourceLoc loc = {})
      : Expr(ExprKind::Binary, std::move(loc)), op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
};

struct UnaryExpr final : Expr {
  UnaryOp op;
  ExprPtr operand;

  UnaryExpr(UnaryOp op, ExprPtr operand, SourceLoc loc = {})
      : Expr(ExprKind::Unary, std::move(loc)), op(op), operand(std::move(operand)) {}
};

struct CallExpr final : Expr {
  std::string callee;
  std::vector<ExprPtr> args;

  CallExpr(std::string callee, std::vector<ExprPtr> args, SourceLoc loc = {})
      : Expr(ExprKind::Call, std::move(loc)), callee(std::move(callee)), args(std::move(args)) {}
};

struct RawExpr : Expr {
  std::string text;

  explicit RawExpr(std::string text, SourceLoc loc = {}) : Expr(ExprKind::Raw, std::move(loc)), text(std::move(text)) {}
};

struct BasePtrExpr final : RawExpr {
  explicit BasePtrExpr(SourceLoc loc = {}) : RawExpr("@base", std::move(loc)) { kind = ExprKind::BasePtr; }
};

struct TopPtrExpr final : RawExpr {
  explicit TopPtrExpr(SourceLoc loc = {}) : RawExpr("@top", std::move(loc)) { kind = ExprKind::TopPtr; }
};

struct BytesInWordExpr final : RawExpr {
  explicit BytesInWordExpr(SourceLoc loc = {}) : RawExpr("@biw", std::move(loc)) { kind = ExprKind::BytesInWord; }
};

struct StructLitExpr final : Expr {
  std::vector<ExprPtr> fields;

  explicit StructLitExpr(std::vector<ExprPtr> fields, SourceLoc loc = {})
      : Expr(ExprKind::StructLit, std::move(loc)), fields(std::move(fields)) {}
};

struct FieldAccessExpr final : Expr {
  std::string varName;
  int fieldIndex;

  FieldAccessExpr(std::string varName, int fieldIndex, SourceLoc loc = {})
      : Expr(ExprKind::FieldAccess, std::move(loc)), varName(std::move(varName)), fieldIndex(fieldIndex) {}
};

struct MemoryLoadExpr final : Expr {
  MemoryWidth width;
  ExprPtr addrExpr;

  MemoryLoadExpr(MemoryWidth width, ExprPtr addrExpr, SourceLoc loc = {})
      : Expr(ExprKind::MemoryLoad, std::move(loc)), width(width), addrExpr(std::move(addrExpr)) {}

  MemoryLoadExpr(int shape, ExprPtr addrExpr, SourceLoc loc = {})
      : MemoryLoadExpr(shape == 1 ? MemoryWidth::Word : MemoryWidth::Word32, std::move(addrExpr), std::move(loc)) {}
};

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
  SharedMemoryStore,
  SharedMemoryLoad,
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;

  virtual ~Stmt() = default;

protected:
  explicit Stmt(StmtKind kind, SourceLoc loc = {}) : kind(kind), loc(std::move(loc)) {}
};

struct Block {
  std::vector<StmtPtr> stmts;
  SourceLoc loc;
};

struct BlockStmt final : Stmt {
  BlockPtr block;

  explicit BlockStmt(BlockPtr block, SourceLoc loc = {})
      : Stmt(StmtKind::Block, std::move(loc)), block(std::move(block)) {}
};

struct VarDeclStmt final : Stmt {
  std::string name;
  std::optional<int> shape;
  ExprPtr init;

  VarDeclStmt(std::string name, ExprPtr init = nullptr, std::optional<int> shape = std::nullopt, SourceLoc loc = {})
      : Stmt(StmtKind::VarDecl, std::move(loc)), name(std::move(name)), shape(shape), init(std::move(init)) {}
};

struct AssignStmt final : Stmt {
  ExprPtr target;
  ExprPtr value;

  AssignStmt(ExprPtr target, ExprPtr value, SourceLoc loc = {})
      : Stmt(StmtKind::Assign, std::move(loc)), target(std::move(target)), value(std::move(value)) {}

  AssignStmt(std::string target, ExprPtr value, SourceLoc loc = {})
      : AssignStmt(std::make_shared<VarRefExpr>(std::move(target), loc), std::move(value), std::move(loc)) {}
};

struct ReturnStmt final : Stmt {
  ExprPtr value;

  explicit ReturnStmt(ExprPtr value = nullptr, SourceLoc loc = {})
      : Stmt(StmtKind::Return, std::move(loc)), value(std::move(value)) {}
};

struct IfStmt final : Stmt {
  ExprPtr condition;
  BlockPtr thenBranch;
  BlockPtr elseBranch;

  IfStmt(ExprPtr condition, BlockPtr thenBranch, BlockPtr elseBranch = nullptr, SourceLoc loc = {})
      : Stmt(StmtKind::If, std::move(loc)), condition(std::move(condition)), thenBranch(std::move(thenBranch)),
        elseBranch(std::move(elseBranch)) {}
};

struct WhileStmt final : Stmt {
  ExprPtr condition;
  BlockPtr body;

  WhileStmt(ExprPtr condition, BlockPtr body, SourceLoc loc = {})
      : Stmt(StmtKind::While, std::move(loc)), condition(std::move(condition)), body(std::move(body)) {}
};

struct ExprStmt final : Stmt {
  ExprPtr expr;

  explicit ExprStmt(ExprPtr expr, SourceLoc loc = {})
      : Stmt(StmtKind::ExprStmt, std::move(loc)), expr(std::move(expr)) {}
};

struct CommentStmt final : Stmt {
  std::string text;

  explicit CommentStmt(std::string text, SourceLoc loc = {})
      : Stmt(StmtKind::Comment, std::move(loc)), text(std::move(text)) {}
};

struct BreakStmt final : Stmt {
  explicit BreakStmt(SourceLoc loc = {}) : Stmt(StmtKind::Break, std::move(loc)) {}
};

struct ContinueStmt final : Stmt {
  explicit ContinueStmt(SourceLoc loc = {}) : Stmt(StmtKind::Continue, std::move(loc)) {}
};

struct MemoryStoreStmt final : Stmt {
  MemoryWidth width;
  ExprPtr srcExpr;
  ExprPtr destExpr;

  MemoryStoreStmt(MemoryWidth width, ExprPtr srcExpr, ExprPtr destExpr, SourceLoc loc = {})
      : Stmt(StmtKind::MemoryStore, std::move(loc)), width(width), srcExpr(std::move(srcExpr)),
        destExpr(std::move(destExpr)) {}

  MemoryStoreStmt(ExprPtr srcExpr, ExprPtr destExpr, SourceLoc loc = {})
      : MemoryStoreStmt(MemoryWidth::Word, std::move(srcExpr), std::move(destExpr), std::move(loc)) {}
};

struct SharedMemoryStoreStmt final : Stmt {
  MemoryWidth width;
  ExprPtr srcExpr;
  ExprPtr destExpr;

  SharedMemoryStoreStmt(MemoryWidth width, ExprPtr srcExpr, ExprPtr destExpr, SourceLoc loc = {})
      : Stmt(StmtKind::SharedMemoryStore, std::move(loc)), width(width), srcExpr(std::move(srcExpr)),
        destExpr(std::move(destExpr)) {}
};

struct SharedMemoryLoadStmt final : Stmt {
  MemoryWidth width;
  std::string target;
  ExprPtr addrExpr;

  SharedMemoryLoadStmt(MemoryWidth width, std::string target, ExprPtr addrExpr, SourceLoc loc = {})
      : Stmt(StmtKind::SharedMemoryLoad, std::move(loc)), width(width), target(std::move(target)),
        addrExpr(std::move(addrExpr)) {}
};

struct DefineStmt final : Stmt {
  std::string name;
  std::string value;

  DefineStmt(std::string name, std::string value, SourceLoc loc = {})
      : Stmt(StmtKind::Define, std::move(loc)), name(std::move(name)), value(std::move(value)) {}
};

struct StructFieldAssignStmt final : Stmt {
  std::string structName;
  int fieldIndex;
  int fieldCount;
  ExprPtr value;

  StructFieldAssignStmt(std::string structName, int fieldIndex, int fieldCount, ExprPtr value, SourceLoc loc = {})
      : Stmt(StmtKind::StructFieldAssign, std::move(loc)), structName(std::move(structName)), fieldIndex(fieldIndex),
        fieldCount(fieldCount), value(std::move(value)) {}
};

struct Param {
  std::string name;
  std::size_t shape = 1;
};

struct Function {
  std::string name;
  std::vector<Param> params;
  BlockPtr body;
  SourceLoc loc;
  bool isInline = false;
};

struct Program {
  std::vector<StmtPtr> globals;
  std::vector<StmtPtr> arrayInits;
  std::vector<std::shared_ptr<Function>> functions;
};

} // namespace pancake

#endif // C2PANCAKE_IR_H
