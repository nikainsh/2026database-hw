/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/operator/aggregate_operators.h"

#include "common/log.h"

static std::string aggregate_name(const Expression *expr)
{
  return expr->name().empty() ? expr->describe() : expr->name();
}

// ---------------------------------------------------------------------------
// ScalarGroupBy
// ---------------------------------------------------------------------------

RC ScalarGroupByOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }

  emitted_ = false;
  states_.clear();
  for (Expression *expr : select_list_) {
    if (expr->expr_type() != ExprType::AGGREGATE) {
      LOG_WARN("bare column '%s' is not allowed without group by", expr->describe().c_str());
      return RC::UNSUPPORTED;
    }
    const AggregateExpression *aggregate = static_cast<const AggregateExpression *>(expr);
    AggregateState              state;
    RC                          rc = state.init(aggregate->func(), aggregate->arg(), aggregate_name(expr));
    if (OB_FAIL(rc)) {
      return rc;
    }
    states_.push_back(state);
  }

  RC rc = children_[0]->open();
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 把子算子的数据全部累加起来
  while (true) {
    Tuple *child_tuple = nullptr;
    rc               = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }
    for (AggregateState &state : states_) {
      rc = state.accumulate(*child_tuple);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  return RC::SUCCESS;
}

RC ScalarGroupByOperator::next(Tuple *&tuple)
{
  if (emitted_) {
    return RC::RECORD_EOF;
  }
  emitted_ = true;

  tuple_.clear();
  for (AggregateState &state : states_) {
    Value value;
    RC    rc = state.result(value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    tuple_.append(value, ColumnRef());
  }
  tuple = &tuple_;
  return RC::SUCCESS;
}

std::string ScalarGroupByOperator::describe() const
{
  std::string text = "scalar_group_by(";
  for (size_t i = 0; i < select_list_.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += select_list_[i]->describe();
  }
  return text + ")";
}

// ---------------------------------------------------------------------------
// HashGroupBy
// ---------------------------------------------------------------------------

RC HashGroupByOperator::make_group_key(const Tuple &tuple, std::string &key)
{
  key.clear();
  for (Expression *expr : group_by_) {
    Value value;
    RC    rc = expr->evaluate(tuple, value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    key += std::to_string(static_cast<int>(value.type()));
    key += "|";
    key += value.to_string();
    key += ";";
  }
  return RC::SUCCESS;
}

RC HashGroupByOperator::open()
{
  if (children_.empty() || group_by_.empty()) {
    return RC::INTERNAL;
  }

  index_.clear();
  groups_.clear();
  cursor_ = 0;

  RC rc = children_[0]->open();
  if (OB_FAIL(rc)) {
    return rc;
  }

  while (true) {
    Tuple *child_tuple = nullptr;
    rc               = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }

    std::string key;
    rc = make_group_key(*child_tuple, key);
    if (OB_FAIL(rc)) {
      return rc;
    }

    auto iter = index_.find(key);
    if (iter == index_.end()) {
      Group group;
      group.sample = *child_tuple;
      for (Expression *expr : select_list_) {
        if (expr->expr_type() != ExprType::AGGREGATE) {
          continue;
        }
        const AggregateExpression *aggregate = static_cast<const AggregateExpression *>(expr);
        AggregateState              state;
        rc = state.init(aggregate->func(), aggregate->arg(), aggregate_name(expr));
        if (OB_FAIL(rc)) {
          return rc;
        }
        group.states.push_back(state);
      }
      groups_.push_back(group);
      index_[key] = groups_.size() - 1;
      iter        = index_.find(key);
    }

    Group &group = groups_[iter->second];
    for (AggregateState &state : group.states) {
      rc = state.accumulate(*child_tuple);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
  }
  return RC::SUCCESS;
}

RC HashGroupByOperator::next(Tuple *&tuple)
{
  if (cursor_ >= groups_.size()) {
    return RC::RECORD_EOF;
  }

  Group &group   = groups_[cursor_++];
  size_t state_at = 0;

  tuple_.clear();
  for (Expression *expr : select_list_) {
    Value value;
    if (expr->expr_type() == ExprType::AGGREGATE) {
      RC rc = group.states[state_at++].result(value);
      if (OB_FAIL(rc)) {
        return rc;
      }
    } else {
      /// 分组列：直接取组内代表行的值（Stmt 已保证非聚合项都是分组列）
      RC rc = expr->evaluate(group.sample, value);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    tuple_.append(value, ColumnRef());
  }

  tuple = &tuple_;
  return RC::SUCCESS;
}

std::string HashGroupByOperator::describe() const
{
  std::string text = "hash_group_by(";
  for (size_t i = 0; i < select_list_.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += select_list_[i]->describe();
  }
  text += " by ";
  for (size_t i = 0; i < group_by_.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += group_by_[i]->describe();
  }
  return text + ")";
}
