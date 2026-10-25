/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/operator/aggregate_operators.h"

#include <cmath>

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
  if (child_opened_) return RC::INVALID_ARGUMENT;
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

  RC rc = children_[0]->open_tracked();
  if (OB_FAIL(rc)) {
    return rc;
  }
  child_opened_ = true;
  auto fail = [&](RC error) { close(); return error; };

  /// 把子算子的数据全部累加起来
  while (true) {
    Tuple *child_tuple = nullptr;
    rc               = children_[0]->next_tracked(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      return fail(rc);
    }
    for (AggregateState &state : states_) {
      rc = state.accumulate(*child_tuple);
      if (OB_FAIL(rc)) {
        return fail(rc);
      }
    }
  }
  return RC::SUCCESS;
}

RC ScalarGroupByOperator::next(Tuple *&tuple)
{
  if (!child_opened_) return RC::INTERNAL;
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

RC ScalarGroupByOperator::close()
{
  RC rc = RC::SUCCESS;
  if (child_opened_) {
    rc = children_[0]->close_tracked();
    child_opened_ = false;
  }
  states_.clear();
  tuple_.clear();
  emitted_ = false;
  return rc;
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
    std::string bytes;
    if (value.type() == AttrType::CHARS) {
      bytes = value.get_string();
    } else {
      // Value::to_string 是面向显示的，会舍入浮点数；不能用于分组判等。
      if (value.type() == AttrType::FLOAT) {
        if (!std::isfinite(value.get_float())) return RC::INVALID_ARGUMENT;
        if (value.get_float() == 0.0f) value = Value(0.0f); // -0 与 +0 同组。
      }
      const int length = attr_type_fixed_len(value.type());
      if (length <= 0) return RC::INVALID_ARGUMENT;
      bytes.resize(static_cast<size_t>(length));
      rc = value.pack_into(&bytes[0], length);
      if (OB_FAIL(rc)) return rc;
    }
    key += std::to_string(static_cast<int>(value.type())) + ":";
    key += std::to_string(bytes.size()) + ":";
    key.append(bytes);
  }
  return RC::SUCCESS;
}

RC HashGroupByOperator::open()
{
  if (child_opened_) return RC::INVALID_ARGUMENT;
  if (children_.empty() || group_by_.empty()) {
    return RC::INTERNAL;
  }

  index_.clear();
  groups_.clear();
  cursor_ = 0;

  RC rc = children_[0]->open_tracked();
  if (OB_FAIL(rc)) {
    return rc;
  }
  child_opened_ = true;
  auto fail = [&](RC error) { close(); return error; };

  while (true) {
    Tuple *child_tuple = nullptr;
    rc               = children_[0]->next_tracked(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      return fail(rc);
    }

    std::string key;
    rc = make_group_key(*child_tuple, key);
    if (OB_FAIL(rc)) {
      return fail(rc);
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
          return fail(rc);
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
        return fail(rc);
      }
    }
  }
  return RC::SUCCESS;
}

RC HashGroupByOperator::next(Tuple *&tuple)
{
  if (!child_opened_) return RC::INTERNAL;
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

RC HashGroupByOperator::close()
{
  RC rc = RC::SUCCESS;
  if (child_opened_) {
    rc = children_[0]->close_tracked();
    child_opened_ = false;
  }
  index_.clear();
  groups_.clear();
  tuple_.clear();
  cursor_ = 0;
  return rc;
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
