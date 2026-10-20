/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/optimizer/optimizer.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

#include "common/log.h"
#include "sql/operator/aggregate_operators.h"
#include "sql/operator/dml_operators.h"
#include "sql/operator/relation_operators.h"
#include "sql/operator/scan_operators.h"

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

static void collect_conjuncts(Expression *expr, std::vector<Expression *> &out)
{
  if (expr == nullptr) {
    return;
  }
  if (expr->expr_type() == ExprType::CONJUNCTION) {
    const ConjunctionExpression *conjunct = static_cast<const ConjunctionExpression *>(expr);
    if (conjunct->is_and()) {
      for (const auto &child : conjunct->children()) {
        collect_conjuncts(child.get(), out);
      }
      return;
    }
  }
  out.push_back(expr);
}

static void collect_table_gets(LogicalOperator &node, std::vector<TableGetOperator *> &gets)
{
  if (node.kind() == LogicalOperatorKind::TABLE_GET) {
    gets.push_back(static_cast<TableGetOperator *>(&node));
  }
  for (const auto &child : node.children()) {
    collect_table_gets(*child, gets);
  }
}

/// 表达式涉及哪些表
static std::set<int> involved_tables(const Expression *expr)
{
  std::vector<ColumnRef> refs;
  expr->collect_column_refs(refs);
  std::set<int> tables;
  for (const ColumnRef &ref : refs) {
    if (ref.table_index >= 0) {
      tables.insert(ref.table_index);
    }
  }
  return tables;
}

std::string LogicalPlan::to_string() const
{
  if (root == nullptr) {
    return "(empty plan)";
  }
  std::ostringstream oss;
  root->explain(oss);
  return oss.str();
}

// ---------------------------------------------------------------------------
// 逻辑计划
// ---------------------------------------------------------------------------

RC LogicalPlanGenerator::create_select(SelectStmt &stmt, LogicalPlan &plan, std::unique_ptr<LogicalOperator> &root)
{
  /// 先给 from 里每张表建一个 table_get
  std::vector<std::unique_ptr<TableGetOperator>> gets;
  for (size_t i = 0; i < stmt.tables().size(); ++i) {
    gets.push_back(std::make_unique<TableGetOperator>(stmt.tables()[i], static_cast<int>(i)));
  }

  std::unique_ptr<LogicalOperator> scan;
  if (gets.size() == 1) {
    scan = std::move(gets.front());
  } else {
    auto join = std::make_unique<JoinOperator>();
    for (auto &get : gets) {
      join->add_child(std::move(get));
    }
    scan = std::move(join);
  }

  /// where 条件先整体挂在 predicate 上，下推规则会把它拆到各张表
  if (stmt.filter() != nullptr) {
    std::unique_ptr<PredicateOperator> predicate =
        std::make_unique<PredicateOperator>(std::vector<Expression *>{stmt.filter()});
    predicate->add_child(std::move(scan));
    scan = std::move(predicate);
  }

  /// 聚合与投影互斥：有聚合就交给聚合算子，否则用投影算子按 select 列表求值
  std::vector<Expression *> select_list;
  for (const auto &expr : stmt.select_list()) {
    select_list.push_back(expr.get());
  }

  if (stmt.has_aggregate() || !stmt.group_by().empty()) {
    std::vector<Expression *> group_by;
    for (const auto &expr : stmt.group_by()) {
      group_by.push_back(expr.get());
    }
    auto aggregate = std::make_unique<AggregateOperator>(std::move(select_list), std::move(group_by));
    aggregate->add_child(std::move(scan));
    root = std::move(aggregate);
  } else {
    auto project = std::make_unique<ProjectOperator>(std::move(select_list));
    project->add_child(std::move(scan));
    root = std::move(project);
  }
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create(Stmt &stmt, LogicalPlan &plan)
{
  std::unique_ptr<LogicalOperator> root;

  switch (stmt.kind()) {
    case Stmt::Kind::SELECT: {
      RC rc = create_select(*stmt.select_stmt(), plan, root);
      if (OB_FAIL(rc)) {
        return rc;
      }
      break;
    }
    case Stmt::Kind::DELETE_STMT: {
      auto *delete_stmt = static_cast<DeleteStmt *>(&stmt);
      auto  get         = std::make_unique<TableGetOperator>(delete_stmt->table(), 0);
      std::unique_ptr<LogicalOperator> scan(get.release());

      if (delete_stmt->filter() != nullptr) {
        auto predicate = std::make_unique<PredicateOperator>(
            std::vector<Expression *>{delete_stmt->filter()});
        predicate->add_child(std::move(scan));
        scan = std::move(predicate);
      }

      auto op = std::make_unique<DeleteLogicalOperator>(delete_stmt->table());
      op->add_child(std::move(scan));
      root = std::move(op);
      break;
    }
    case Stmt::Kind::UPDATE: {
      auto *update_stmt = static_cast<UpdateStmt *>(&stmt);
      auto  get         = std::make_unique<TableGetOperator>(update_stmt->table(), 0);
      std::unique_ptr<LogicalOperator> scan(get.release());

      if (update_stmt->filter() != nullptr) {
        auto predicate = std::make_unique<PredicateOperator>(
            std::vector<Expression *>{update_stmt->filter()});
        predicate->add_child(std::move(scan));
        scan = std::move(predicate);
      }

      auto op = std::make_unique<UpdateLogicalOperator>(update_stmt->table(), update_stmt->assignments());
      op->add_child(std::move(scan));
      root = std::move(op);
      break;
    }
    case Stmt::Kind::INSERT: {
      auto *insert_stmt = static_cast<InsertStmt *>(&stmt);
      root = std::make_unique<InsertLogicalOperator>(insert_stmt->table(), insert_stmt->values_list());
      break;
    }
    default:
      LOG_WARN("statement has no logical plan: %s", stmt.name());
      return RC::UNSUPPORTED;
  }

  plan.root = std::move(root);
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 优化规则
// ---------------------------------------------------------------------------

RC Optimizer::optimize()
{
  if (plan_.root == nullptr) {
    return RC::INTERNAL;
  }

  RC rc = predicate_pushdown();
  if (OB_FAIL(rc)) {
    return rc;
  }
  return comparison_simplification();
}

/**
 * @brief 谓词下推
 * @details 把一个 AND 串起来的条件拆开：只涉及一张表的条件下推到该表的 table_get 上，
 *          需要多张表才能算的条件留在 predicate 里。这样扫描阶段就能尽量少地输出行，
 *          也为后面的索引选择创造条件。
 */
RC Optimizer::predicate_pushdown()
{
  std::vector<TableGetOperator *> gets;
  collect_table_gets(*plan_.root, gets);
  if (gets.empty()) {
    return RC::SUCCESS;
  }

  std::vector<PredicateOperator *> predicates;
  std::function<void(LogicalOperator &)> walk = [&](LogicalOperator &node) {
    if (node.kind() == LogicalOperatorKind::PREDICATE) {
      predicates.push_back(static_cast<PredicateOperator *>(&node));
    }
    for (const auto &child : node.children()) {
      walk(*child);
    }
  };
  walk(*plan_.root);

  for (PredicateOperator *predicate : predicates) {
    std::vector<Expression *> conjuncts;
    for (Expression *conjunct : predicate->conjuncts()) {
      collect_conjuncts(conjunct, conjuncts);
    }

    std::vector<Expression *> residual;
    for (Expression *conjunct : conjuncts) {
      const std::set<int> tables = involved_tables(conjunct);
      if (tables.size() == 1) {
        const int  table_index = *tables.begin();
        auto       iter        = std::find_if(gets.begin(), gets.end(),
                                              [table_index](TableGetOperator *get) {
                                                return get->table_index() == table_index;
                                              });
        if (iter != gets.end()) {
          (*iter)->add_filter(conjunct);
          continue;
        }
      }
      residual.push_back(conjunct);
    }

    predicate->set_conjuncts(std::move(residual));
  }
  return RC::SUCCESS;
}

/**
 * @brief 比较式化简
 * @details 1) 把 "常量 op 常量" 直接算成 TRUE/FALSE（miniob 的 ComparisonSimplification）；
 *          2) 恒真的条件删掉；3) 恒假的条件把算子标记成空扫描；
 *          4) 顺带把条件规范成「列 op 常量」，索引选择只看这一种形式。
 */
RC Optimizer::comparison_simplification()
{
  std::vector<TableGetOperator *> gets;
  collect_table_gets(*plan_.root, gets);

  std::vector<PredicateOperator *> predicates;
  std::function<void(LogicalOperator &)> walk = [&](LogicalOperator &node) {
    if (node.kind() == LogicalOperatorKind::PREDICATE) {
      predicates.push_back(static_cast<PredicateOperator *>(&node));
    }
    for (const auto &child : node.children()) {
      walk(*child);
    }
  };
  walk(*plan_.root);

  auto simplify_list = [&](std::vector<Expression *> &list, bool &always_false) -> RC {
    std::vector<Expression *> kept;
    for (Expression *expr : list) {
      Expression *current = expr;

      if (current->expr_type() == ExprType::COMPARISON) {
        ComparisonExpression *comparison = static_cast<ComparisonExpression *>(current);
        comparison->normalize_operands();

        if (comparison->is_constant_compare()) {
          Tuple  empty;
          Value  folded;
          RC     rc = current->evaluate(empty, folded);
          if (OB_FAIL(rc)) {
            return rc;
          }
          if (folded.type() != AttrType::BOOLEAN) {
            return RC::SCHEMA_FIELD_TYPE_MISMATCH;
          }
          if (folded.get_bool()) {
            continue;  /// 恒真：直接去掉
          }
          always_false = true;
          continue;
        }
      }
      kept.push_back(current);
    }
    list = std::move(kept);
    return RC::SUCCESS;
  };

  for (TableGetOperator *get : gets) {
    std::vector<Expression *> filters = get->filters();
    bool                      constant_false = false;
    RC                      rc = simplify_list(filters, constant_false);
    if (OB_FAIL(rc)) {
      return rc;
    }
    get->set_filters(filters);
    if (constant_false) {
      get->mark_always_false();
    }
  }

  for (PredicateOperator *predicate : predicates) {
    std::vector<Expression *> conjuncts = predicate->conjuncts();
    bool                      constant_false = false;
    RC                      rc = simplify_list(conjuncts, constant_false);
    if (OB_FAIL(rc)) {
      return rc;
    }
    predicate->set_conjuncts(conjuncts);
    if (constant_false) {
      predicate->mark_always_false();
    }
  }
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 物理计划
// ---------------------------------------------------------------------------

bool PhysicalPlanGenerator::choose_index_access(TableGetOperator &get, Index *&index, IndexScanRange &range)
{
  index    = nullptr;
  range    = IndexScanRange();
  bool equal = false;

  for (Expression *filter : get.filters()) {
    if (filter->expr_type() != ExprType::COMPARISON) {
      continue;
    }
    ComparisonExpression *comparison = static_cast<ComparisonExpression *>(filter);
    const Expression     *left       = comparison->left();
    const Expression     *right      = comparison->right();
    if (left == nullptr || right == nullptr) {
      continue;
    }
    if (left->expr_type() != ExprType::COLUMN || right->expr_type() != ExprType::CONST) {
      continue;
    }

    const ColumnExpression *column = static_cast<const ColumnExpression *>(left);
    if (column->table_index() != get.table_index()) {
      continue;
    }
    const TableMeta &meta = get.table()->table_meta();
    if (column->column_index() < 0 || column->column_index() >= static_cast<int>(meta.columns().size())) {
      continue;
    }
    Index *candidate = get.table()->index_of_field(meta.columns()[column->column_index()].name());
    if (candidate == nullptr) {
      continue;
    }

    const Value &value = static_cast<const ConstExpression *>(right)->value();
    bool         usable = true;
    switch (comparison->comp()) {
      case CompOp::EQUAL_TO:
        range.has_low = range.has_high = true;
        range.low = range.high          = value;
        range.low_inclusive = range.high_inclusive = true;
        equal                                     = true;
        break;
      case CompOp::LESS_THAN:
        range.has_high      = true;
        range.high          = value;
        range.high_inclusive = false;
        break;
      case CompOp::LESS_EQUAL:
        range.has_high      = true;
        range.high          = value;
        range.high_inclusive = true;
        break;
      case CompOp::GREAT_THAN:
        range.has_low      = true;
        range.low          = value;
        range.low_inclusive = false;
        break;
      case CompOp::GREAT_EQUAL:
        range.has_low      = true;
        range.low          = value;
        range.low_inclusive = true;
        break;
      default: usable = false; break;  /// != 用不上索引
    }

    if (usable && (index == nullptr || equal)) {
      index = candidate;
    }
  }
  return index != nullptr;
}

RC PhysicalPlanGenerator::create(LogicalOperator &logical, std::unique_ptr<PhysicalOperator> &plan)
{
  switch (logical.kind()) {
    case LogicalOperatorKind::TABLE_GET: {
      TableGetOperator &get = *static_cast<TableGetOperator *>(&logical);
      if (get.always_false()) {
        plan = std::make_unique<StringListOperator>();  /// 空结果，连扫描都省了
        return RC::SUCCESS;
      }

      Index *index = nullptr;
      IndexScanRange range;
      if (choose_index_access(get, index, range)) {
        plan = std::make_unique<IndexScanOperator>(get.table(), index, get.filters(), range, get.table_index());
      } else {
        plan = std::make_unique<TableScanOperator>(get.table(), get.filters(), get.table_index());
      }
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::PREDICATE: {
      PredicateOperator &predicate = *static_cast<PredicateOperator *>(&logical);
      if (predicate.always_false()) {
        plan = std::make_unique<StringListOperator>();
        return RC::SUCCESS;
      }
      if (predicate.children().empty()) {
        return RC::INTERNAL;
      }

      std::unique_ptr<PhysicalOperator> child;
      RC                                rc = create(*predicate.children()[0], child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (predicate.conjuncts().empty()) {
        plan = std::move(child);  /// 条件全被下推或化简掉了，不必再加一层过滤
        return RC::SUCCESS;
      }
      auto filter = std::make_unique<FilterPhysicalOperator>(predicate.conjuncts());
      filter->add_child(std::move(child));
      plan = std::move(filter);
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::JOIN: {
      JoinOperator &join = *static_cast<JoinOperator *>(&logical);
      if (join.children().empty()) {
        return RC::INTERNAL;
      }

      /// 左深树：先把第一个孩子的计划建好，再依次和后面的孩子做嵌套循环连接
      std::unique_ptr<PhysicalOperator> left;
      RC                                rc = create(*join.children()[0], left);
      if (OB_FAIL(rc)) {
        return rc;
      }
      for (size_t i = 1; i < join.children().size(); ++i) {
        std::unique_ptr<PhysicalOperator> right;
        rc = create(*join.children()[i], right);
        if (OB_FAIL(rc)) {
          return rc;
        }
        auto nested = std::make_unique<NestedLoopJoinOperator>(nullptr);
        nested->add_child(std::move(left));
        nested->add_child(std::move(right));
        left = std::move(nested);
      }
      plan = std::move(left);
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::PROJECT: {
      ProjectOperator &project = *static_cast<ProjectOperator *>(&logical);
      std::unique_ptr<PhysicalOperator> child;
      RC rc = create(*project.children()[0], child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      auto op = std::make_unique<ProjectPhysicalOperator>(project.project_list());
      op->add_child(std::move(child));
      plan = std::move(op);
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::AGGREGATE: {
      AggregateOperator &aggregate = *static_cast<AggregateOperator *>(&logical);
      std::unique_ptr<PhysicalOperator> child;
      RC rc = create(*aggregate.children()[0], child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      std::unique_ptr<PhysicalOperator> op;
      if (aggregate.group_by().empty()) {
        op = std::make_unique<ScalarGroupByOperator>(aggregate.select_list());
      } else {
        op = std::make_unique<HashGroupByOperator>(aggregate.select_list(), aggregate.group_by());
      }
      op->add_child(std::move(child));
      plan = std::move(op);
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::INSERT: {
      InsertLogicalOperator &insert = *static_cast<InsertLogicalOperator *>(&logical);
      plan = std::make_unique<InsertOperator>(insert.table(), insert.rows());
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::DELETE_OP: {
      DeleteLogicalOperator &delete_op = *static_cast<DeleteLogicalOperator *>(&logical);
      if (delete_op.children().empty()) {
        return RC::INTERNAL;
      }
      std::unique_ptr<PhysicalOperator> child;
      RC                                rc = create(*delete_op.children()[0], child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      auto op = std::make_unique<DeleteOperator>(delete_op.table());
      op->add_child(std::move(child));
      plan = std::move(op);
      return RC::SUCCESS;
    }

    case LogicalOperatorKind::UPDATE: {
      UpdateLogicalOperator &update_op = *static_cast<UpdateLogicalOperator *>(&logical);
      if (update_op.children().empty()) {
        return RC::INTERNAL;
      }
      std::unique_ptr<PhysicalOperator> child;
      RC                                rc = create(*update_op.children()[0], child);
      if (OB_FAIL(rc)) {
        return rc;
      }
      auto op = std::make_unique<UpdateOperator>(update_op.table(), update_op.assignments());
      op->add_child(std::move(child));
      plan = std::move(op);
      return RC::SUCCESS;
    }

    default:
      LOG_WARN("no physical plan for logical operator %s",
               logical_operator_kind_to_string(logical.kind()));
      return RC::UNSUPPORTED;
  }
}
