#ifndef C2PANCAKE_IR_H
#define C2PANCAKE_IR_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pancake {
struct SourceLoc {
  std::string file;
  unsigned line = 0;
  unsigned col = 0;
};

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
  // default copy constructor
  Expr(const Expr &) = default;
  // default copy assignment operator
  auto operator=(const Expr &) -> Expr & = default;
  // default move constructor
  Expr(Expr &&) = default;
  // default move assignment operator
  auto operator=(Expr &&) -> Expr & = default;

protected:
  Expr(ExprKind k, SourceLoc l = {}) : kind(k), loc(std::move(std::move(l))) {}
};

struct IntLitExpr : Expr {
  int64_t value;
  IntLitExpr(int64_t v, SourceLoc l = {})
      : Expr(ExprKind::IntLit, std::move(l)), value(v) {}
};

struct VarRefExpr : Expr {
  std::string name;
  VarRefExpr(std::string n, SourceLoc l = {})
      : Expr(ExprKind::VarRef, std::move(l)), name(std::move(std::move(n))) {}
};

struct BinaryExpr : Expr {
  BinOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(BinOp o, ExprPtr l, ExprPtr r, SourceLoc loc = {})
      : Expr(ExprKind::Binary, std::move(loc)), op(o),
        lhs(std::move(std::move(l))), rhs(std::move(std::move(r))) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(UnaryOp o, ExprPtr e, SourceLoc l = {})
      : Expr(ExprKind::Unary, std::move(l)), op(o),
        operand(std::move(std::move(e))) {}
};

struct CallExpr : Expr {
  std::string callee;
  std::vector<ExprPtr> args;
  CallExpr(std::string c, std::vector<ExprPtr> a, SourceLoc l = {})
      : Expr(ExprKind::Call, std::move(l)), callee(std::move(std::move(c))),
        args(std::move(std::move(a))) {}
};

// raw source text fallback
struct RawExpr : Expr {
  std::string text;
  RawExpr(std::string t, SourceLoc l = {})
      : Expr(ExprKind::Raw, std::move(l)), text(std::move(std::move(t))) {}
};

struct BasePtrExpr : RawExpr {
  BasePtrExpr(SourceLoc l = {}) : RawExpr("@base", std::move(l)) {}
};

struct TopPtrExpr : RawExpr {
  TopPtrExpr(SourceLoc l = {}) : RawExpr("@top", std::move(l)) {}
};

struct BytesInWordExpr : RawExpr {
  BytesInWordExpr(SourceLoc l = {}) : RawExpr("@biw", std::move(l)) {}
};

struct StructLitExpr : Expr {
  std::vector<ExprPtr> fields;
  StructLitExpr(std::vector<ExprPtr> f, SourceLoc l = {})
      : Expr(ExprKind::StructLit, std::move(l)),
        fields(std::move(std::move(f))) {}
};

struct FieldAccessExpr : Expr {
  std::string varName;
  int fieldIndex;
  FieldAccessExpr(std::string v, int idx, SourceLoc l = {})
      : Expr(ExprKind::FieldAccess, std::move(l)),
        varName(std::move(std::move(v))), fieldIndex(idx) {}
};

struct MemoryLoadExpr : Expr {
  int shape;
  ExprPtr addrExpr;
  MemoryLoadExpr(int shape, ExprPtr addr, SourceLoc l = {})
      : Expr(ExprKind::MemoryLoad, std::move(l)), shape(shape),
        addrExpr(std::move(std::move(addr))) {}
};

struct ArrayIndexExpr : Expr {
  ExprPtr idx;
  ExprPtr step;
  ArrayIndexExpr(ExprPtr i, ExprPtr s, SourceLoc l = {})
      : Expr(ExprKind::ArrayIndex, std::move(l)), idx(std::move(std::move(i))),
        step(std::move(std::move(s))) {}
};

struct ArrayLoadExpr : Expr {
  int baseSlot;
  ExprPtr indexExpr; // this should be a ArrayIndexExpr!!!
  ArrayLoadExpr(int bs, ExprPtr idx, SourceLoc l = {})
      : Expr(ExprKind::ArrayLoad, std::move(l)), baseSlot(bs),
        indexExpr(std::move(std::move(idx))) {}
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
  ArrayStore,        // helper for array element assignment
  SharedMemoryStore, // TODO
  SharedMemoryLoad,  // TODO
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;
  virtual ~Stmt() = default;
  // default copy constructor
  Stmt(const Stmt &) = default;
  // default copy assignment operator
  auto operator=(const Stmt &) -> Stmt & = default;
  // default move constructor
  Stmt(Stmt &&) = default;
  // default move assignment operator
  auto operator=(Stmt &&) -> Stmt & = default;

protected:
  Stmt(StmtKind k, SourceLoc l = {}) : kind(k), loc(std::move(std::move(l))) {}
};

struct Block {
  std::vector<StmtPtr> stmts;
  SourceLoc loc;
};

struct BlockStmt : Stmt {
  BlockPtr block;
  SourceLoc loc;
  BlockStmt(BlockPtr b, SourceLoc l = {})
      : Stmt(StmtKind::Block, std::move(l)), block(std::move(std::move(b))) {}
};

struct VarDeclStmt : Stmt {
  std::string name;
  std::optional<int> shape; // shape hint for call-initialized vars
  ExprPtr init;
  VarDeclStmt(std::string n, ExprPtr i = nullptr,
              std::optional<int> sh = std::nullopt, SourceLoc l = {})
      : Stmt(StmtKind::VarDecl, std::move(l)), name(std::move(std::move(n))),
        shape(sh), init(std::move(std::move(i))) {}
};

struct AssignStmt : Stmt {
  ExprPtr target;
  ExprPtr value;
  AssignStmt(ExprPtr t, ExprPtr v, SourceLoc l = {})
      : Stmt(StmtKind::Assign, std::move(l)), target(std::move(std::move(t))),
        value(std::move(std::move(v))) {}
  AssignStmt(const std::string &t, ExprPtr v, const SourceLoc &l = {})
      : Stmt(StmtKind::Assign, l), target(std::make_shared<VarRefExpr>(t, l)),
        value(std::move(std::move(v))) {}
};

struct ReturnStmt : Stmt {
  ExprPtr value;
  ReturnStmt(ExprPtr v = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::Return, std::move(l)), value(std::move(std::move(v))) {}
};

struct IfStmt : Stmt {
  ExprPtr condition;
  BlockPtr thenBranch;
  BlockPtr elseBranch;
  IfStmt(ExprPtr c, BlockPtr t, BlockPtr e = nullptr, SourceLoc l = {})
      : Stmt(StmtKind::If, std::move(l)), condition(std::move(std::move(c))),
        thenBranch(std::move(std::move(t))),
        elseBranch(std::move(std::move(e))) {}
};

struct WhileStmt : Stmt {
  ExprPtr condition;
  BlockPtr body;
  WhileStmt(ExprPtr c, BlockPtr b, SourceLoc l = {})
      : Stmt(StmtKind::While, std::move(l)), condition(std::move(std::move(c))),
        body(std::move(std::move(b))) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  ExprStmt(ExprPtr e, SourceLoc l = {})
      : Stmt(StmtKind::ExprStmt, std::move(l)), expr(std::move(std::move(e))) {}
};

struct CommentStmt : Stmt {
  std::string text;
  CommentStmt(std::string t, SourceLoc l = {})
      : Stmt(StmtKind::Comment, std::move(l)), text(std::move(std::move(t))) {}
};

struct BreakStmt : Stmt {
  BreakStmt(SourceLoc l = {}) : Stmt(StmtKind::Break, std::move(l)) {}
};

struct ContinueStmt : Stmt {
  ContinueStmt(SourceLoc l = {}) : Stmt(StmtKind::Continue, std::move(l)) {}
};

struct MemoryStoreStmt : Stmt {
  ExprPtr srcExpr;
  ExprPtr destExpr;
  MemoryStoreStmt(ExprPtr src, ExprPtr dest, SourceLoc l = {})
      : Stmt(StmtKind::MemoryStore, std::move(l)),
        srcExpr(std::move(std::move(src))),
        destExpr(std::move(std::move(dest))) {}
};

struct ArrayStoreStmt : Stmt {
  int baseSlot;
  ExprPtr indexExpr; // this should be a ArrayIndexExpr!!!
  ExprPtr valueExpr;
  ArrayStoreStmt(int bs, ExprPtr idx, ExprPtr val, SourceLoc l = {})
      : Stmt(StmtKind::ArrayStore, std::move(l)), baseSlot(bs),
        indexExpr(std::move(std::move(idx))),
        valueExpr(std::move(std::move(val))) {}
};

struct DefineStmt : Stmt {
  std::string name;
  int64_t value;
  DefineStmt(std::string n, int64_t v, SourceLoc l = {})
      : Stmt(StmtKind::Define, std::move(l)), name(std::move(std::move(n))),
        value(v) {}
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
        structName(std::move(std::move(sn))), fieldIndex(idx),
        fieldCount(count), value(std::move(std::move(v))) {}
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
