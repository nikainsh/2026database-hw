/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "sql/operator/dml_operators.h"

#include "common/log.h"

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

RC InsertOperator::open()
{
  affected_rows_ = 0;

  for (const std::vector<Value> &row : rows_) {
    Record record;
    RC     rc = table_->table_meta().make_record(row, record);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to build record. table=%s, rc=%s", table_->table_name().c_str(), strrc(rc));
      return rc;
    }

    rc = table_->insert_record(record);
    if (OB_FAIL(rc)) {
      /// 本实现没有事务，已经插进去的行会留在表里；这里只把错误如实报出
      LOG_WARN("failed to insert record. table=%s, rc=%s", table_->table_name().c_str(), strrc(rc));
      return rc;
    }
    affected_rows_ += 1;
  }
  return RC::SUCCESS;
}

RC InsertOperator::next(Tuple *&) { return RC::RECORD_EOF; }

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------

RC DeleteOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }
  affected_rows_ = 0;
  finished_      = false;
  return children_[0]->open();
}

RC DeleteOperator::next(Tuple *&tuple)
{
  if (finished_) {
    return RC::RECORD_EOF;
  }

  while (true) {
    Tuple *child_tuple = nullptr;
    RC    rc          = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      finished_ = true;
      return RC::RECORD_EOF;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!child_tuple->has_rid) {
      LOG_ERROR("the scan tuple carries no rid, cannot delete");
      return RC::RECORD_INVALID_RID;
    }

    rc = table_->delete_record(child_tuple->rid);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to delete record. table=%s, rid=%s, rc=%s",
                table_->table_name().c_str(), child_tuple->rid.to_string().c_str(), strrc(rc));
      return rc;
    }
    affected_rows_ += 1;
  }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

RC UpdateOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }
  affected_rows_ = 0;
  finished_      = false;
  return children_[0]->open();
}

RC UpdateOperator::next(Tuple *&tuple)
{
  if (finished_) {
    return RC::RECORD_EOF;
  }

  const TableMeta &meta = table_->table_meta();

  while (true) {
    Tuple *child_tuple = nullptr;
    RC    rc          = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      finished_ = true;
      return RC::RECORD_EOF;
    }
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!child_tuple->has_rid) {
      LOG_ERROR("the scan tuple carries no rid, cannot update");
      return RC::RECORD_INVALID_RID;
    }

    /// 读出整条记录，改掉指定的列，再写回去（记录是定长的，索引由 Table 负责维护）
    Record record;
    rc   = table_->get_record(child_tuple->rid, record);
    if (OB_FAIL(rc)) {
      return rc;
    }

    for (const auto &assignment : assignments_) {
      const ColumnMeta &column = meta.columns()[assignment.first];
      Value            staged;
      rc = assignment.second.cast_to(column.type(), staged);
      if (OB_FAIL(rc)) {
        return rc;
      }
      rc = staged.pack_into(record.data() + column.offset(), column.data_len());
      if (OB_FAIL(rc)) {
        return rc;
      }
    }

    rc = table_->update_record(child_tuple->rid, record);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to update record. table=%s, rid=%s, rc=%s",
                table_->table_name().c_str(), child_tuple->rid.to_string().c_str(), strrc(rc));
      return rc;
    }
    affected_rows_ += 1;
  }
}
