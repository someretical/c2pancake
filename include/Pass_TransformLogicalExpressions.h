#ifndef C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS_H
#define C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS_H

#include "Pipeline.h"

#include <cstdint>
#include <functional>

/*
lift the condition expressions out of if statements and hoist them to temporary variables, so that the if statement
conditions are pure variable references. same thing applied to while loops and return statements
*/
namespace pancake::pass_hoist_condition_expressions {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "HoistLogicalSideEffectExpressions";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_hoist_condition_expressions

namespace pancake::pass_rewrite_array_indexing {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteArrayIndexing";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_rewrite_array_indexing

namespace pancake::pass_rewrite_struct_stabs {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "RewriteStructStabs";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_rewrite_struct_stabs

/*
Turn all nested expressions with side effects into temporary variables, so that all expressions are pure variable
references
*/
namespace pancake::pass_lower_nested_expressions {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "LowerNestedExpressions";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};

struct BuiltExpr {
  llvm::SmallVector<std::string, 8> pre_stmts;
  clang::QualType final_expr_type;
  std::string final_expr;
  explicit BuiltExpr(llvm::SmallVector<std::string, 8> pre_stmts, std::string final_expr,
                     clang::QualType final_expr_type)
      : pre_stmts(std::move(pre_stmts)), final_expr_type(final_expr_type), final_expr(std::move(final_expr)) {}
};

enum class Usage : uint8_t {
  /**
   * @brief Compute the expression's resulting value
   *
   * Used when: The caller needs the result
   *
   * Example: x = a + b, foo(expr), int y = (b = 3)
   */
  Value,

  /**
   * @brief Compute the reusable description of the storage location of the expression. Note this is very specifically
   * not supposed to return a pointer!
   *
   * Used when: The caller needs to read/write memory at that location
   *
   * Example: x = 3 (lhs), *p = v, ++a->b
   */
  Place,

  /**
   * @brief Execute the expression only for its side effects; ignore its value
   *
   * Used when: The result is thrown away
   *
   * Example: a = 3;, foo(expr);, ++a;
   */
  Effect
};
struct BuildExprCtx {
  clang::Expr *expr;
  Usage usage_kind;
  bool deref_force_extract; // force the extraction of the next deref into a temp var
  using AssignedToPair =
      std::pair<std::reference_wrapper<const std::string>, std::reference_wrapper<const clang::QualType>>;
  std::optional<AssignedToPair> assigned_to; // if this expression is being assigned to a variable, this is the name and
                                             // type of that variable. This is only relevant for init list expressions
  explicit BuildExprCtx(clang::Expr *expr, Usage usage_kind, bool deref_force_extract,
                        std::optional<AssignedToPair> assigned_to)
      : expr(expr), usage_kind(usage_kind), deref_force_extract(deref_force_extract),
        assigned_to(std::move(assigned_to)) {}
};

auto GetUsage(const clang::Expr *expr) -> Usage;
} // namespace pancake::pass_lower_nested_expressions

namespace pancake::pass_simplify_double_negation {
class Consumer : public C2PancakePass {
public:
  using C2PancakePass::C2PancakePass; // inherit constructor
  void HandleTranslationUnit(clang::ASTContext &Ctx) override;
};

class Action : public PipelineAction<Consumer> {
public:
  explicit Action(PipelineActionCtx &ctx) : PipelineAction<Consumer>(ctx) {
    ctx.action_name = "SimplifyDoubleNegation";
    ctx.failure_behaviour = FailureBehaviour::RepeatPass;
    ctx.action_type = PipelineActionType::Rewriter;
  }
};
} // namespace pancake::pass_simplify_double_negation

#endif // C2PANCAKE_PASS_TRANSFORM_LOGICAL_EXPRESSIONS