/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/operator/scan_operators.h"

#include "common/log.h"

RC TableScanOperator::open()
{
  affected_rows_ = 0;
  RC rc          = table_->start_scan(scanner_);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to start table scan. table=%s, rc=%s", table_->table_name().c_str(), strrc(rc));
  }
  return rc;
}

RC TableScanOperator::close()
{
  scanner_.restart();
  return RC::SUCCESS;
}

RC TableScanOperator::fill_tuple(const Record &record)
{
  tuple_.clear();
  const TableMeta &meta = table_->table_meta();
  for (size_t i = 0; i < meta.columns().size(); ++i) {
    Value value;
    RC    rc = meta.get_field_value(record, meta.columns()[i], value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    tuple_.append(value, ColumnRef(table_index_, static_cast<int>(i)));
  }
  tuple_.rid     = record.rid();
  tuple_.has_rid = true;
  return RC::SUCCESS;
}

RC TableScanOperator::matches(const Tuple &tuple, bool &ok)
{
  ok = true;
  for (Expression *filter : filters_) {
    Value result;
    RC    rc = filter->evaluate(tuple, result);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (result.type() != AttrType::BOOLEAN) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    if (!result.get_bool()) {
      ok = false;
      return RC::SUCCESS;
    }
  }
  return RC::SUCCESS;
}

RC TableScanOperator::next(Tuple *&tuple)
{
  while (true) {
    RC rc = scanner_.next(record_);
    if (rc == RC::RECORD_EOF) {
      return RC::RECORD_EOF;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }

    rc = fill_tuple(record_);
    if (OB_FAIL(rc)) {
      return rc;
    }

    bool ok = true;
    rc   = matches(tuple_, ok);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!ok) {
      continue;
    }

    tuple = &tuple_;
    return RC::SUCCESS;
  }
}

// ---------------------------------------------------------------------------
// IndexScan
// ---------------------------------------------------------------------------

RC IndexScanOperator::open()
{
  affected_rows_ = 0;
  return index_->make_scanner(scanner_,
                              range_.has_low ? &range_.low : nullptr, range_.low_inclusive,
                              range_.has_high ? &range_.high : nullptr, range_.high_inclusive);
}

RC IndexScanOperator::close()
{
  scanner_.restart();
  return RC::SUCCESS;
}

RC IndexScanOperator::fill_tuple(const Record &record)
{
  tuple_.clear();
  const TableMeta &meta = table_->table_meta();
  for (size_t i = 0; i < meta.columns().size(); ++i) {
    Value value;
    RC    rc = meta.get_field_value(record, meta.columns()[i], value);
    if (OB_FAIL(rc)) {
      return rc;
    }
    tuple_.append(value, ColumnRef(table_index_, static_cast<int>(i)));
  }
  tuple_.rid     = record.rid();
  tuple_.has_rid = true;
  return RC::SUCCESS;
}

RC IndexScanOperator::matches(const Tuple &tuple, bool &ok)
{
  ok = true;
  for (Expression *filter : filters_) {
    Value result;
    RC    rc = filter->evaluate(tuple, result);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (result.type() != AttrType::BOOLEAN) {
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
    if (!result.get_bool()) {
      ok = false;
      return RC::SUCCESS;
    }
  }
  return RC::SUCCESS;
}

RC IndexScanOperator::next(Tuple *&tuple)
{
  while (true) {
    RID rid;
    RC  rc = scanner_.next(key_, rid);
    if (rc == RC::RECORD_EOF) {
      return RC::RECORD_EOF;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }

    Record record;
    rc   = table_->get_record(rid, record);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to fetch record by rid. table=%s, rid=%s, rc=%s",
                table_->table_name().c_str(), rid.to_string().c_str(), strrc(rc));
      return rc;
    }

    rc = fill_tuple(record);
    if (OB_FAIL(rc)) {
      return rc;
    }

    /// 索引条件比下推条件更宽时（例如 a<5 and a<3），剩余条件还要再过滤一次
    bool ok = true;
    rc   = matches(tuple_, ok);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!ok) {
      continue;
    }

    tuple = &tuple_;
    return RC::SUCCESS;
  }
}
