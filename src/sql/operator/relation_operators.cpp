/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/operator/relation_operators.h"

#include "common/log.h"
#include "common/string_util.h"

// ---------------------------------------------------------------------------
// Predicate
// ---------------------------------------------------------------------------

RC FilterPhysicalOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }
  return children_[0]->open();
}

RC FilterPhysicalOperator::next(Tuple *&tuple)
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }

  while (true) {
    Tuple *child_tuple = nullptr;
    RC    rc           = children_[0]->next(child_tuple);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool pass = true;
    for (Expression *conjunct : conjuncts_) {
      Value result;
      rc = conjunct->evaluate(*child_tuple, result);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (result.type() != AttrType::BOOLEAN) {
        LOG_WARN("predicate must evaluate to a boolean. expr=%s", conjunct->describe().c_str());
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      if (!result.get_bool()) {
        pass = false;
        break;
      }
    }

    if (pass) {
      tuple = child_tuple;
      return RC::SUCCESS;
    }
  }
}

// ---------------------------------------------------------------------------
// NestedLoopJoin
// ---------------------------------------------------------------------------

RC NestedLoopJoinOperator::open()
{
  if (children_.size() != 2) {
    LOG_ERROR("nested loop join needs exactly 2 children, got %d", (int)children_.size());
    return RC::INTERNAL;
  }

  left_ready_ = false;
  left_eof_   = false;
  return children_[0]->open();
}

RC NestedLoopJoinOperator::close()
{
  RC rc = RC::SUCCESS;
  for (auto &child : children_) {
    RC one = child->close();
    if (OB_FAIL(one)) {
      rc = one;
    }
  }
  left_ready_ = false;
  left_eof_   = false;
  return rc;
}

RC NestedLoopJoinOperator::next(Tuple *&tuple)
{
  while (true) {
    if (left_eof_) {
      return RC::RECORD_EOF;
    }

    /// 取一条左边的行，然后重新打开右边
    if (!left_ready_) {
      Tuple *left = nullptr;
      RC    rc    = children_[0]->next(left);
      if (rc == RC::RECORD_EOF) {
        left_eof_ = true;
        return RC::RECORD_EOF;
      }
      if (OB_FAIL(rc)) {
        return rc;
      }
      left_tuple_ = *left;  /// 复制一份：子算子会复用它的缓冲区
      left_ready_ = true;

      rc = children_[1]->open();
      if (OB_FAIL(rc)) {
        return rc;
      }
    }

    Tuple *right = nullptr;
    RC    rc   = children_[1]->next(right);
    if (rc == RC::RECORD_EOF) {
      children_[1]->close();
      left_ready_ = false;
      continue;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }

    joined_.clear();
    joined_.append_all(left_tuple_);
    joined_.append_all(*right);
    joined_.rid        = left_tuple_.rid;
    joined_.has_rid    = left_tuple_.has_rid;

    if (on_condition_ != nullptr) {
      Value result;
      rc = on_condition_->evaluate(joined_, result);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (!result.get_bool()) {
        continue;
      }
    }

    tuple = &joined_;
    return RC::SUCCESS;
  }
}

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

RC ProjectPhysicalOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }
  return children_[0]->open();
}

RC ProjectPhysicalOperator::next(Tuple *&tuple)
{
  Tuple *child_tuple = nullptr;
  RC    rc          = children_[0]->next(child_tuple);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  tuple_.clear();
  for (Expression *expr : project_list_) {
    Value value;
    rc = expr->evaluate(*child_tuple, value);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to evaluate select item. expr=%s, rc=%s", expr->describe().c_str(), strrc(rc));
      return rc;
    }
    tuple_.append(value, ColumnRef());
  }

  /// rid 透传：delete/update 的子算子需要靠它定位记录
  tuple_.rid      = child_tuple->rid;
  tuple_.has_rid  = child_tuple->has_rid;

  tuple = &tuple_;
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// StringList / Explain
// ---------------------------------------------------------------------------

RC StringListOperator::open()
{
  index_ = 0;
  return RC::SUCCESS;
}

RC StringListOperator::next(Tuple *&tuple)
{
  if (index_ >= rows_.size()) {
    return RC::RECORD_EOF;
  }

  tuple_.clear();
  for (const Value &value : rows_[index_]) {
    tuple_.append(value, ColumnRef());
  }
  ++index_;
  tuple = &tuple_;
  return RC::SUCCESS;
}

RC ExplainOperator::open()
{
  lines_ = split(plan_text_, '\n');
  index_ = 0;
  return RC::SUCCESS;
}

RC ExplainOperator::next(Tuple *&tuple)
{
  if (index_ >= lines_.size()) {
    return RC::RECORD_EOF;
  }
  tuple_.clear();
  tuple_.append(Value(lines_[index_]), ColumnRef());
  ++index_;
  tuple = &tuple_;
  return RC::SUCCESS;
}
