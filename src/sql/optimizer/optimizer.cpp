/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/optimizer/optimizer.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

#include "common/log.h"
#include "sql/operator/aggregate_operators.h"
#include "sql/operator/dml_operators.h"
#include "sql/operator/hash_join_operator.h"
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
  rc = reorder_joins_by_cardinality();
  if (OB_FAIL(rc)) {
    return rc;
  }
  return comparison_simplification();
}

/**
 * @brief 谓词下推
 * @details 把一个 AND 串起来的条件拆开：
 *          1) 只涉及一张表的条件下推到该表的 table_get 上，扫描阶段就尽量少输出行，
 *             也为后面的索引选择创造条件；
 *          2) 两侧各出一张表列的等值条件（连接条件）挂到 join 节点上，物理计划阶段
 *             才能用它把「内表全扫」换成「按连接键的索引查找」；
 *          3) 其余需要多张表才能算的条件留在 predicate 里。
 */
/// 找出树里所有连接节点
static void collect_joins(LogicalOperator &node, std::vector<JoinOperator *> &joins)
{
  if (node.kind() == LogicalOperatorKind::JOIN) {
    joins.push_back(static_cast<JoinOperator *>(&node));
  }
  for (const auto &child : node.children()) {
    collect_joins(*child, joins);
  }
}

/// 判断一个等值比较是不是「两侧分别是两张表的列」，即可以当连接条件用
static bool is_two_table_equality(Expression *expr, std::set<int> &tables)
{
  if (expr->expr_type() != ExprType::COMPARISON) {
    return false;
  }
  ComparisonExpression *comparison = static_cast<ComparisonExpression *>(expr);
  if (comparison->comp() != CompOp::EQUAL_TO || comparison->left() == nullptr || comparison->right() == nullptr) {
    return false;
  }
  if (comparison->left()->expr_type() != ExprType::COLUMN || comparison->right()->expr_type() != ExprType::COLUMN) {
    return false;
  }
  tables = involved_tables(expr);
  return tables.size() == 2;
}

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

  std::vector<JoinOperator *> joins;
  collect_joins(*plan_.root, joins);

  /// 找到能同时算出这几张表列的那个连接节点
  auto join_covering = [&joins](const std::set<int> &tables) {
    for (JoinOperator *join : joins) {
      std::vector<TableGetOperator *> covered;
      collect_table_gets(*join, covered);
      bool all = true;
      for (int table_index : tables) {
        if (std::find_if(covered.begin(), covered.end(), [table_index](TableGetOperator *get) {
              return get->table_index() == table_index;
            }) == covered.end()) {
          all = false;
          break;
        }
      }
      if (all) {
        return join;
      }
    }
    return static_cast<JoinOperator *>(nullptr);
  };

  for (PredicateOperator *predicate : predicates) {
    std::vector<Expression *> conjuncts;
    for (Expression *conjunct : predicate->conjuncts()) {
      collect_conjuncts(conjunct, conjuncts);
    }

    std::vector<Expression *> residual;
    for (Expression *conjunct : conjuncts) {
      std::set<int> join_tables;
      if (is_two_table_equality(conjunct, join_tables)) {
        JoinOperator *join = join_covering(join_tables);
        if (join != nullptr) {
          join->add_on_condition(conjunct);
          continue;
        }
      }

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
 * @brief 连接顺序
 * @details 嵌套循环连接的代价 ≈ 左边的行数 × 右边每行的代价，所以把基数小的表放到
 *          左边当驱动表。元组是「每列自带来源」的，换顺序不会改变表达式求值的结果，
 *          输出列的顺序由 select 列表决定，因此这一步是安全的。
 */
RC Optimizer::reorder_joins_by_cardinality()
{
  std::vector<JoinOperator *> joins;
  collect_joins(*plan_.root, joins);

  for (JoinOperator *join : joins) {
    if (join->children().size() < 2) {
      continue;
    }
    auto rows_of = [](const std::unique_ptr<LogicalOperator> &child) -> int64_t {
      if (child->kind() != LogicalOperatorKind::TABLE_GET) {
        return std::numeric_limits<int64_t>::max();
      }
      Table *table = static_cast<TableGetOperator *>(child.get())->table();
      int64_t rows = 0;
      return OB_SUCC(table->record_count(rows)) ? rows : 0;
    };
    std::stable_sort(join->mutable_children().begin(), join->mutable_children().end(),
                     [&rows_of](const std::unique_ptr<LogicalOperator> &a,
                                 const std::unique_ptr<LogicalOperator> &b) {
                       return rows_of(a) < rows_of(b);
                     });
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

namespace {

/// 代价单位 = "全表扫描多处理一行" 的相对开销。系数由 tests/bench_sql.cpp 在
/// 20 万行 / 8KB 页的数据集上标定（见 docs/optimization-log.md）：
///   table_scan 200000 行 ≈ 11.7 ms  => 58.5 ns/行
///   index_scan + 回表 2000 行 ≈ 1.02 ms => 509 ns/行  => 回表一行 ≈ 扫 8.7 行
constexpr double kIndexRowCost   = 8.7;
constexpr double kIndexTreePages = 4.0;  /// 树高带来的固定页读

bool as_double(const Value &value, double &out)
{
  Value casted;
  if (OB_FAIL(value.cast_to(AttrType::FLOAT, casted))) {
    return false;
  }
  out = casted.get_float();
  return true;
}

/// 全表扫描的固定页开销：一页大约放多少行，由记录长度推出。小表上这项主导，
/// 少了它代价模型会在几行的表上错误地放弃索引。
int rows_per_page_of(const TableMeta &table_meta)
{
  return std::max(1, (BP_PAGE_SIZE - static_cast<int>(sizeof(SlottedPageHeader)) - static_cast<int>(sizeof(int32_t))) /
                          (table_meta.record_size() + static_cast<int>(sizeof(int32_t))));
}

/// 估算某个候选索引能命中多少条索引项；返回 -1 表示无法估算
int64_t estimate_index_rows(Table *table, const Index *index, bool has_equal, const IndexScanRange &range)
{
  const IndexStats *stats = table->table_meta().index_stats(index->index_meta().index_name);
  if (stats == nullptr || !stats->valid || stats->entries <= 0) {
    return -1;
  }

  if (has_equal) {
    const int64_t ndv = stats->distinct_keys > 0 ? stats->distinct_keys : 1;
    const int64_t matched = stats->entries / ndv;
    return matched > 0 ? matched : 1;
  }

  /// 范围条件：用最小/最大键做线性插值（只对数值列成立）
  const AttrType type = index->index_column() != nullptr ? index->index_column()->type() : AttrType::UNDEFINED;
  if (type != AttrType::INT && type != AttrType::BIGINT && type != AttrType::FLOAT) {
    return -1;
  }
  Value min_value, max_value;
  if (OB_FAIL(index->decode_key(stats->min_key, min_value)) || OB_FAIL(index->decode_key(stats->max_key, max_value))) {
    return -1;
  }
  double low_edge = 0.0, high_edge = 0.0;
  if (!as_double(min_value, low_edge) || !as_double(max_value, high_edge) || high_edge <= low_edge) {
    return -1;
  }

  double lower = low_edge, upper = high_edge;
  if (range.has_low) {
    double bound = 0.0;
    if (!as_double(range.low, bound)) {
      return -1;
    }
    lower = std::max(lower, bound);
  }
  if (range.has_high) {
    double bound = 0.0;
    if (!as_double(range.high, bound)) {
      return -1;
    }
    upper = std::min(upper, bound);
  }
  if (upper <= lower) {
    return 0;  /// 区间落在键范围之外，扫描必然为空
  }
  const double fraction = (upper - lower) / (high_edge - low_edge);
  int64_t      matched  = static_cast<int64_t>(fraction * static_cast<double>(stats->entries));
  return matched < 0 ? 0 : matched;
}

/// 索引嵌套循环连接：内表按外层连接键做索引查找
struct JoinLookup
{
  Expression            *condition     = nullptr;  /// 挂回这一层 NLJ 上，用于结果复核
  ColumnRef              outer_column;             /// 外层元组里当查询键的那一列
  Index                 *index         = nullptr;
};

/// 连接键两侧类型必须一致：value_to_key 会做 cast_to，跨类型（尤其 int/bigint 混用）
/// 有可能截断而查不到本该匹配的行。宁可不走索引，也不能漏答案。
bool join_key_types_match(const ColumnExpression &outer, const ColumnMeta &inner)
{
  return outer.result_type() == inner.type();
}

bool choose_join_index_lookup(TableGetOperator &get, const std::vector<Expression *> &conditions,
                             const std::set<int> &outer_tables, JoinLookup &out)
{
  if (OB_FAIL(get.table()->ensure_index_stats())) {
    return false;
  }
  int64_t table_rows = 0;
  get.table()->record_count(table_rows);
  const TableMeta &meta        = get.table()->table_meta();
  const double     scan_cost   = static_cast<double>(table_rows > 0 ? table_rows : 1) + rows_per_page_of(meta);

  for (Expression *condition : conditions) {
    if (condition->expr_type() != ExprType::COMPARISON) {
      continue;
    }
    ComparisonExpression *comparison = static_cast<ComparisonExpression *>(condition);
    if (comparison->comp() != CompOp::EQUAL_TO || comparison->left() == nullptr || comparison->right() == nullptr) {
      continue;
    }
    if (comparison->left()->expr_type() != ExprType::COLUMN || comparison->right()->expr_type() != ExprType::COLUMN) {
      continue;
    }
    const ColumnExpression *left  = static_cast<const ColumnExpression *>(comparison->left());
    const ColumnExpression *right = static_cast<const ColumnExpression *>(comparison->right());

    /// inner 是本表（被查找的一侧），outer 已经在左深树的左边
    const ColumnExpression *inner = nullptr;
    const ColumnExpression *outer = nullptr;
    if (left->table_index() == get.table_index() && outer_tables.count(right->table_index()) > 0) {
      inner = left;
      outer = right;
    } else if (right->table_index() == get.table_index() && outer_tables.count(left->table_index()) > 0) {
      inner = right;
      outer = left;
    } else {
      continue;
    }
    if (inner->column_index() < 0 || inner->column_index() >= static_cast<int>(meta.columns().size())) {
      continue;
    }
    Index *index = get.table()->index_of_field(meta.columns()[inner->column_index()].name());
    if (index == nullptr || index->index_column() == nullptr) {
      continue;
    }
    if (!join_key_types_match(*outer, *index->index_column())) {
      continue;
    }

    const int64_t matched = estimate_index_rows(get.table(), index, true, IndexScanRange());
    if (matched <= 0) {
      continue;  /// 估不出来就不赌
    }
    if (static_cast<double>(matched) * kIndexRowCost + kIndexTreePages >= scan_cost) {
      continue;  /// 每行都全表扫更划算（内表很小或连接键选择率太差）
    }

    out.condition     = condition;
    out.outer_column  = outer->ref();
    out.index         = index;
    return true;
  }
  return false;
}

// A conservative bound for the retained build tuples, using unfiltered table
// counts and their Cartesian product. Do not promise that this is process RSS.
// Unknown/oversized inputs keep the streaming nested-loop plan instead of
// selecting a hash table already known to exceed its estimated-storage budget.
bool hash_build_fits(const std::vector<TableGetOperator *> &gets, const std::vector<HashJoinKey> &keys)
{
  const size_t budget = HashJoinOperator::DEFAULT_MAX_BUILD_BYTES;
  size_t bytes = sizeof(Tuple) + sizeof(std::string) + sizeof(std::vector<Tuple>) + 4 * sizeof(void *);
  auto charge = [&](size_t amount) {
    if (bytes > budget || amount > budget - bytes) return false;
    bytes += amount;
    return true;
  };
  size_t found_keys = 0;
  size_t rows = 1;
  for (TableGetOperator *get : gets) {
    const auto &columns = get->table()->table_meta().columns();
    for (size_t i = 0; i < columns.size(); ++i) {
      const auto &column = columns[i];
      if (!charge(sizeof(Value) + sizeof(ColumnRef))) return false;
      if (column.type() == AttrType::CHARS && !charge(static_cast<size_t>(column.data_len()))) return false;
      for (const auto &key : keys) {
        if (key.left == ColumnRef(get->table_index(), static_cast<int>(i))) {
          if (!charge(HashJoinOperator::KEY_PART_OVERHEAD) || !charge(static_cast<size_t>(column.data_len()))) return false;
          ++found_keys;
        }
      }
    }
    int64_t count = 0;
    if (OB_FAIL(get->table()->record_count(count)) || count < 0) return false;
    if (get->always_false()) count = 0;
    if (rows == 0 || count == 0) rows = 0;
    else {
      if (static_cast<uint64_t>(count) > budget / rows) return false;
      rows *= static_cast<size_t>(count);
    }
  }
  return found_keys == keys.size() && bytes <= budget && rows <= budget / bytes;
}

bool choose_hash_join(const std::vector<Expression *> &conditions, const std::set<int> &left_tables,
                      const std::set<int> &right_tables, const std::vector<TableGetOperator *> &build_gets,
                      std::vector<HashJoinKey> &keys)
{
  // These are mandatory AND conjuncts moved to Join by predicate_pushdown.
  // Never descend into OR to extract an equality that is not mandatory.
  for (Expression *condition : conditions) {
    if (condition->expr_type() != ExprType::COMPARISON) continue;
    const auto *comparison = static_cast<const ComparisonExpression *>(condition);
    if (comparison->comp() != CompOp::EQUAL_TO || comparison->left() == nullptr || comparison->right() == nullptr ||
        comparison->left()->expr_type() != ExprType::COLUMN || comparison->right()->expr_type() != ExprType::COLUMN) continue;
    const auto *left = static_cast<const ColumnExpression *>(comparison->left());
    const auto *right = static_cast<const ColumnExpression *>(comparison->right());
    if (left_tables.count(right->table_index()) && right_tables.count(left->table_index())) std::swap(left, right);
    if (!left_tables.count(left->table_index()) || !right_tables.count(right->table_index())) continue;
    const AttrType type = left->result_type();
    if (type != right->result_type() ||
        (type != AttrType::INT && type != AttrType::BIGINT && type != AttrType::CHARS)) continue;
    const HashJoinKey key{left->ref(), right->ref(), type};
    const bool duplicate = std::any_of(keys.begin(), keys.end(), [&](const HashJoinKey &existing) {
      return existing.left == key.left && existing.right == key.right && existing.type == key.type;
    });
    if (!duplicate) keys.push_back(key);
  }
  return !keys.empty() && hash_build_fits(build_gets, keys);
}

/// select count(*) from 单表 且没有任何过滤条件：答案就是堆文件头里的记录数，
/// 不必真的去扫一遍（InnoDB 的 count(*) 短路是同一条思路）
bool is_plain_count_star(AggregateOperator &aggregate, TableGetOperator *&get)
{
  get = nullptr;
  if (!aggregate.group_by().empty() || aggregate.select_list().size() != 1) {
    return false;
  }
  Expression *expr = aggregate.select_list()[0];
  if (expr->expr_type() != ExprType::AGGREGATE) {
    return false;
  }
  const AggregateExpression *agg = static_cast<const AggregateExpression *>(expr);
  if (agg->func() != AggFunc::COUNT || agg->arg() != nullptr) {
    return false;  /// 只有 count(*) 成立，count(col) 要跳过 NULL
  }
  if (aggregate.children().size() != 1 || aggregate.children()[0]->kind() != LogicalOperatorKind::TABLE_GET) {
    return false;
  }
  TableGetOperator *candidate = static_cast<TableGetOperator *>(aggregate.children()[0].get());
  if (!candidate->filters().empty() || candidate->always_false()) {
    return false;  /// 有条件就可能过滤掉行，头里的总数不再等于答案
  }
  get = candidate;
  return true;
}

}  // namespace

bool PhysicalPlanGenerator::choose_index_access(TableGetOperator &get, Index *&index, IndexScanRange &range)
{
  index = nullptr;
  range = IndexScanRange();

  /// 每个「有索引的列」是一个候选，各自维护自己的键范围。
  /// 旧实现把所有列的条件写进同一个 range，导致 `id > 2 and cat < 4` 这类查询
  /// 用 id 的索引却带上 cat 的上界，扫描区间被非法收窄而漏行。
  struct Candidate
  {
    int            column_index = -1;
    Index         *index        = nullptr;
    IndexScanRange range;
    bool           has_equal    = false;
  };
  std::vector<Candidate> candidates;

  auto find_candidate = [&candidates](int column_index) {
    for (auto &item : candidates) {
      if (item.column_index == column_index) {
        return &item;
      }
    }
    return static_cast<Candidate *>(nullptr);
  };

  auto value_less = [](const Value &a, const Value &b) {
    int cmp = 0;
    return OB_SUCC(a.compare_to(b, cmp)) && cmp < 0;
  };

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

    Candidate *candidate = find_candidate(column->column_index());
    if (candidate == nullptr) {
      Index *idx = get.table()->index_of_field(meta.columns()[column->column_index()].name());
      if (idx == nullptr) {
        continue;  /// 该列没有索引
      }
      candidates.push_back(Candidate{column->column_index(), idx, IndexScanRange(), false});
      candidate = &candidates.back();
    }

    const Value &value = static_cast<const ConstExpression *>(right)->value();
    switch (comparison->comp()) {
      case CompOp::EQUAL_TO:
        candidate->range.has_low = candidate->range.has_high = true;
        candidate->range.low = candidate->range.high          = value;
        candidate->range.low_inclusive = candidate->range.high_inclusive = true;
        candidate->has_equal                                    = true;
        break;
      case CompOp::LESS_THAN:
      case CompOp::LESS_EQUAL: {
        const bool inclusive = comparison->comp() == CompOp::LESS_EQUAL;
        if (!candidate->range.has_high || value_less(value, candidate->range.high)) {
          candidate->range.has_high      = true;
          candidate->range.high          = value;
          candidate->range.high_inclusive = inclusive;
        }
        break;
      }
      case CompOp::GREAT_THAN:
      case CompOp::GREAT_EQUAL: {
        const bool inclusive = comparison->comp() == CompOp::GREAT_EQUAL;
        if (!candidate->range.has_low || value_less(candidate->range.low, value)) {
          candidate->range.has_low       = true;
          candidate->range.low           = value;
          candidate->range.low_inclusive = inclusive;
        }
        break;
      }
      default: break;  /// != 给不出可用的扫描区间
    }
  }

  /// 代价化选路：估算每个候选索引能命中多少索引项，回表按 kIndexRowCost 计价，
  /// 与「整表扫一遍」比较，只有更便宜才真的走索引。
  /// 估不出来（例如字符列的范围条件）就按"整表那么多行"计，等价于放弃走索引。
  auto score = [](const Candidate &c) {
    if (c.has_equal) return 3;
    if (c.range.has_low && c.range.has_high) return 2;
    if (c.range.has_low || c.range.has_high) return 1;
    return 0;
  };

  int64_t table_rows = 0;
  get.table()->record_count(table_rows);
  if (OB_FAIL(get.table()->ensure_index_stats())) {
    return false;
  }

  Candidate *best        = nullptr;
  int64_t    best_rows   = 0;
  for (auto &c : candidates) {
    if (score(c) == 0) {
      continue;  /// 没有有效区间，这个索引用不上
    }
    const int64_t stats_rows = table_rows > 0 ? table_rows : 1;
    int64_t        matched  = estimate_index_rows(get.table(), c.index, c.has_equal, c.range);
    if (matched < 0) {
      matched = stats_rows;
    }
    if (best == nullptr || matched < best_rows) {
      best      = &c;
      best_rows = matched;
    }
  }
  if (best == nullptr) {
    LOG_DBG("no index candidate for table=%s filters=%d", get.table()->table_name().c_str(),
            static_cast<int>(get.filters().size()));
    return false;
  }

  const double index_cost = static_cast<double>(best_rows) * kIndexRowCost + kIndexTreePages;
  /// 全表扫描的代价 = 逐行处理 + 一个数据页的固定开销
  const double scan_cost =
      static_cast<double>(table_rows > 0 ? table_rows : 1) + rows_per_page_of(get.table()->table_meta());
  LOG_DBG("access path table=%s rows=%lld matched=%lld index_cost=%.1f scan_cost=%.1f",
          get.table()->table_name().c_str(), (long long)table_rows, (long long)best_rows, index_cost, scan_cost);
  if (index_cost >= scan_cost) {
    return false;  /// 全表扫描更便宜
  }

  index = best->index;
  range = best->range;
  return true;
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

      /// 一棵连接树里有哪几张表，决定条件该挂到左深树的哪一层
      auto tables_of = [](LogicalOperator &node) {
        std::vector<TableGetOperator *> covered;
        collect_table_gets(node, covered);
        std::set<int> indexes;
        for (TableGetOperator *get : covered) {
          indexes.insert(get->table_index());
        }
        return indexes;
      };

      std::unique_ptr<PhysicalOperator> left;
      RC                                rc = create(*join.children()[0], left);
      if (OB_FAIL(rc)) {
        return rc;
      }
      std::set<int> outer_tables = tables_of(*join.children()[0]);
      std::vector<Expression *> unattached = join.on_conditions();
      std::vector<TableGetOperator *> build_gets;
      collect_table_gets(*join.children()[0], build_gets);

      for (size_t i = 1; i < join.children().size(); ++i) {
        LogicalOperator &child       = *join.children()[i];
        const std::set<int> child_tables = tables_of(child);

        /// 等两边都能算的时候才挂上来，挂早了元组里还没有对应的列
        std::vector<Expression *> step_conditions;
        for (Expression *condition : join.on_conditions()) {
          const std::set<int> involved = involved_tables(condition);
          bool                ready    = true;
          bool                covers   = false;
          for (int table_index : involved) {
            if (child_tables.count(table_index) > 0) {
              covers = true;
            } else if (outer_tables.count(table_index) == 0) {
              ready = false;
            }
          }
          if (ready && covers) {
            step_conditions.push_back(condition);
            unattached.erase(std::remove(unattached.begin(), unattached.end(), condition), unattached.end());
          }
        }

        std::unique_ptr<PhysicalOperator> right;
        rc = create(child, right);
        if (OB_FAIL(rc)) {
          return rc;
        }

        /// 索引嵌套循环连接：内表本来要全扫，但连接列上有索引且更便宜，
        /// 就改成「按外层这一行的键值查索引」。连接条件仍会在 NLJ 里复核，
        /// 所以这一步只影响快慢，不影响结果。
        bool index_lookup = false;
        if (child.kind() == LogicalOperatorKind::TABLE_GET &&
            right->kind() == PhysicalOperatorKind::TABLE_SCAN) {
          TableGetOperator &get       = *static_cast<TableGetOperator *>(&child);
          JoinLookup        lookup;
          if (choose_join_index_lookup(get, step_conditions, outer_tables, lookup)) {
            auto scan = std::make_unique<IndexScanOperator>(get.table(), lookup.index, get.filters(),
                                                            IndexScanRange(), get.table_index());
            scan->set_join_key(lookup.outer_column);
            right = std::move(scan);
            index_lookup = true;
          }
        }

        // Preserve the existing indexed lookup. A parameterized probe scan
        // cannot be opened once by Hash Join: its key depends on each outer row.
        std::vector<HashJoinKey> hash_keys;
        if (!index_lookup && choose_hash_join(step_conditions, outer_tables, child_tables, build_gets, hash_keys)) {
          auto hash = std::make_unique<HashJoinOperator>(std::move(hash_keys));
          hash->add_child(std::move(left));
          hash->add_child(std::move(right));
          // Keep ALL step conjuncts, including other equality keys. Borrow the
          // Stmt-owned expressions; do not create a short-lived expression tree.
          auto filter = std::make_unique<FilterPhysicalOperator>(std::move(step_conditions));
          filter->add_child(std::move(hash));
          left = std::move(filter);
        } else {
          auto nested = std::make_unique<NestedLoopJoinOperator>(std::move(step_conditions));
          nested->add_child(std::move(left));
          nested->add_child(std::move(right));
          left = std::move(nested);
        }

        for (int table_index : child_tables) {
          outer_tables.insert(table_index);
        }
        collect_table_gets(child, build_gets);
      }

      /// 兜底：挂不上任何一层的连接条件（理论上不该发生）继续在顶层过滤，宁可慢不能漏
      if (!unattached.empty()) {
        LOG_WARN("join condition not attached to any join step, kept as filter. count=%d",
                 static_cast<int>(unattached.size()));
        auto filter = std::make_unique<FilterPhysicalOperator>(unattached);
        filter->add_child(std::move(left));
        left = std::move(filter);
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

      TableGetOperator *count_get = nullptr;
      if (is_plain_count_star(aggregate, count_get)) {
        int64_t rows = 0;
        if (OB_SUCC(count_get->table()->record_count(rows))) {
          auto single = std::make_unique<StringListOperator>();
          single->add_row(std::vector<Value>{Value(rows)});
          plan = std::move(single);
          return RC::SUCCESS;
        }
      }

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
