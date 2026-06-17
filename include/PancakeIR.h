#ifndef C2PANCAKE_IR_H
#define C2PANCAKE_IR_H

#include <clang/Basic/SourceLocation.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pancake {

enum class BinOp : uint8_t {
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

enum class UnaryOp : uint8_t {
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
enum class ExprKind : uint8_t {
  IntLit,
  VarRef,
  Binary,
  Unary,
  Call,
  StructLit,
  FieldAccess,
  MemoryLoad,
  Raw,
  ArrayIndex,
  ArrayLoad,
  BasePtr,
  TopPtr,
  BytesInWord,
};

struct Expr {
  ExprKind kind;
  clang::SourceLocation loc;

  virtual ~Expr() = default;
  Expr(const Expr &) = default;
  auto operator=(const Expr &) -> Expr & = default;
  Expr(Expr &&) = default;
  auto operator=(Expr &&) -> Expr & = default;

protected:
  Expr(ExprKind k, const clang::SourceLocation &loc) : kind(k), loc(loc) {}
};

struct IntLitExpr : Expr {
  int64_t value;

  IntLitExpr(const clang::SourceLocation &loc, int64_t v)
      : Expr(ExprKind::IntLit, loc), value(v) {}
};

struct DeclRefExpr : Expr {
  std::string name;

  DeclRefExpr(const clang::SourceLocation &loc, std::string n)
      : Expr(ExprKind::VarRef, loc), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
  BinOp op;
  ExprPtr lhs, rhs;

  BinaryExpr(const clang::SourceLocation &loc, BinOp o, ExprPtr l, ExprPtr r)
      : Expr(ExprKind::Binary, loc), op(o), lhs(std::move(l)),
        rhs(std::move(r)) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;

  UnaryExpr(const clang::SourceLocation &loc, UnaryOp o, ExprPtr e)
      : Expr(ExprKind::Unary, loc), op(o), operand(std::move(e)) {}
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<ExprPtr> args;

  CallExpr(const clang::SourceLocation &loc, std::string c,
           std::vector<ExprPtr> a)
      : Expr(ExprKind::Call, loc), callee(std::move(c)), args(std::move(a)) {}
};

struct RawExpr : Expr {
  std::string text;

  RawExpr(const clang::SourceLocation &loc, std::string t)
      : Expr(ExprKind::Raw, loc), text(std::move(t)) {}
};

struct BasePtrExpr : RawExpr {
  BasePtrExpr(const clang::SourceLocation &loc) : RawExpr(loc, "@base") {}
};

struct TopPtrExpr : RawExpr {
  TopPtrExpr(const clang::SourceLocation &loc) : RawExpr(loc, "@top") {}
};

struct BytesInWordExpr : RawExpr {
  BytesInWordExpr(const clang::SourceLocation &loc) : RawExpr(loc, "@biw") {}
};

struct StructLitExpr : Expr {
  std::vector<ExprPtr> fields;

  StructLitExpr(const clang::SourceLocation &loc, std::vector<ExprPtr> f)
      : Expr(ExprKind::StructLit, loc), fields(std::move(f)) {}
};

struct FieldAccessExpr : Expr {
  std::string varName;
  int fieldIndex;

  FieldAccessExpr(const clang::SourceLocation &loc, std::string v, int idx)
      : Expr(ExprKind::FieldAccess, loc), varName(std::move(v)),
        fieldIndex(idx) {}
};

struct MemoryLoadExpr : Expr {
  int shape;
  ExprPtr addrExpr;

  MemoryLoadExpr(const clang::SourceLocation &loc, int shape, ExprPtr addr)
      : Expr(ExprKind::MemoryLoad, loc), shape(shape),
        addrExpr(std::move(addr)) {}
};

struct ArrayIndexExpr : Expr {
  ExprPtr idx;
  ExprPtr step;

  ArrayIndexExpr(const clang::SourceLocation &loc, ExprPtr i, ExprPtr s)
      : Expr(ExprKind::ArrayIndex, loc), idx(std::move(i)), step(std::move(s)) {
  }
};

struct ArrayLoadExpr : Expr {
  int baseSlot;
  ExprPtr indexExpr;

  ArrayLoadExpr(const clang::SourceLocation &loc, int bs, ExprPtr idx)
      : Expr(ExprKind::ArrayLoad, loc), baseSlot(bs),
        indexExpr(std::move(idx)) {}
};

// statements
enum class StmtKind : uint8_t {
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
  ArrayStore,
  SharedMemoryStore,
  SharedMemoryLoad,
};

struct Stmt {
  StmtKind kind;
  clang::SourceLocation loc;

  virtual ~Stmt() = default;
  Stmt(const Stmt &) = default;
  auto operator=(const Stmt &) -> Stmt & = default;
  Stmt(Stmt &&) = default;
  auto operator=(Stmt &&) -> Stmt & = default;

protected:
  Stmt(StmtKind k, const clang::SourceLocation &loc) : kind(k), loc(loc) {}
};

struct Block {
  std::vector<StmtPtr> stmts;
  clang::SourceLocation loc;

  Block(const clang::SourceLocation &loc, std::vector<StmtPtr> s)
      : stmts(std::move(s)), loc(loc) {}
};

struct BlockStmt : Stmt {
  BlockPtr block;

  BlockStmt(const clang::SourceLocation &loc, BlockPtr b)
      : Stmt(StmtKind::Block, loc), block(std::move(b)) {}
};

struct VarDeclStmt : Stmt {
  std::string name;
  std::optional<int> shape;
  ExprPtr init;

  VarDeclStmt(const clang::SourceLocation &loc, std::string n, ExprPtr i,
              std::optional<int> sh)
      : Stmt(StmtKind::VarDecl, loc), name(std::move(n)), shape(sh),
        init(std::move(i)) {}
};

struct AssignStmt : Stmt {
  ExprPtr target;
  ExprPtr value;

  AssignStmt(const clang::SourceLocation &loc, ExprPtr t, ExprPtr v)
      : Stmt(StmtKind::Assign, loc), target(std::move(t)), value(std::move(v)) {
  }
};

struct ReturnStmt : Stmt {
  ExprPtr value;

  ReturnStmt(const clang::SourceLocation &loc, ExprPtr v)
      : Stmt(StmtKind::Return, loc), value(std::move(v)) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  BlockPtr thenBranch;
  BlockPtr elseBranch;

  IfStmt(const clang::SourceLocation &loc, ExprPtr c, BlockPtr t, BlockPtr e)
      : Stmt(StmtKind::If, loc), condition(std::move(c)),
        thenBranch(std::move(t)), elseBranch(std::move(e)) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  BlockPtr body;

  WhileStmt(const clang::SourceLocation &loc, ExprPtr c, BlockPtr b)
      : Stmt(StmtKind::While, loc), condition(std::move(c)),
        body(std::move(b)) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;

  ExprStmt(const clang::SourceLocation &loc, ExprPtr e)
      : Stmt(StmtKind::ExprStmt, loc), expr(std::move(e)) {}
};

struct CommentStmt : Stmt {
  std::string text;

  CommentStmt(const clang::SourceLocation &loc, std::string t)
      : Stmt(StmtKind::Comment, loc), text(std::move(t)) {}
};

struct BreakStmt : Stmt {
  BreakStmt(const clang::SourceLocation &loc) : Stmt(StmtKind::Break, loc) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt(const clang::SourceLocation &loc)
      : Stmt(StmtKind::Continue, loc) {}
};

struct MemoryStoreStmt : Stmt {
  ExprPtr srcExpr;
  ExprPtr destExpr;

  MemoryStoreStmt(const clang::SourceLocation &loc, ExprPtr src, ExprPtr dest)
      : Stmt(StmtKind::MemoryStore, loc), srcExpr(std::move(src)),
        destExpr(std::move(dest)) {}
};

struct ArrayStoreStmt : Stmt {
  int baseSlot;
  ExprPtr indexExpr;
  ExprPtr valueExpr;

  ArrayStoreStmt(const clang::SourceLocation &loc, int bs, ExprPtr idx,
                 ExprPtr val)
      : Stmt(StmtKind::ArrayStore, loc), baseSlot(bs),
        indexExpr(std::move(idx)), valueExpr(std::move(val)) {}
};

struct DefineStmt : Stmt {
  std::string name;
  int64_t value;

  DefineStmt(const clang::SourceLocation &loc, std::string n, int64_t v)
      : Stmt(StmtKind::Define, loc), name(std::move(n)), value(v) {}
};

struct StructFieldAssignStmt : Stmt {
  std::string structName;
  int fieldIndex;
  int fieldCount;
  ExprPtr value;

  StructFieldAssignStmt(const clang::SourceLocation &loc, std::string sn,
                        int idx, int count, ExprPtr v)
      : Stmt(StmtKind::StructFieldAssign, loc), structName(std::move(sn)),
        fieldIndex(idx), fieldCount(count), value(std::move(v)) {}
};

// top-level
struct Param {
  std::string name;
  size_t shape = 1;
};

struct Function {
  std::string name;
  std::vector<Param> params;
  std::string returnType;
  BlockPtr body;
  clang::SourceLocation loc;

  Function(const clang::SourceLocation &loc, std::string n,
           std::vector<Param> p, std::string rt, BlockPtr b)
      : name(std::move(n)), params(std::move(p)), returnType(std::move(rt)),
        body(std::move(b)), loc(loc) {}
};

} // namespace pancake

#endif // C2PANCAKE_IR_H