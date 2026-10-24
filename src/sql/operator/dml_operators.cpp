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
  affected_rows_   = 0;
  finished_        = false;
  next_rid_index_  = 0;
  rids_.clear();

  RC rc = children_[0]->open();
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// First collect all RIDs, then close the child scan. Deleting while an
  /// index scanner is still active would hold its shared tree lock and
  /// deadlock against the index's write lock.
  while (true) {
    Tuple *child_tuple = nullptr;
    rc                 = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      children_[0]->close();
      return rc;
    }
    if (!child_tuple->has_rid) {
      children_[0]->close();
      LOG_ERROR("the scan tuple carries no rid, cannot delete");
      return RC::RECORD_INVALID_RID;
    }
    rids_.push_back(child_tuple->rid);
  }

  RC close_rc = children_[0]->close();
  if (OB_FAIL(close_rc)) {
    return close_rc;
  }
  return RC::SUCCESS;
}

RC DeleteOperator::next(Tuple *&tuple)
{
  if (finished_) {
    return RC::RECORD_EOF;
  }

  while (next_rid_index_ < rids_.size()) {
    const RID &rid = rids_[next_rid_index_++];
    RC         rc  = table_->delete_record(rid);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to delete record. table=%s, rid=%s, rc=%s",
                table_->table_name().c_str(), rid.to_string().c_str(), strrc(rc));
      return rc;
    }
    affected_rows_ += 1;
  }

  finished_ = true;
  return RC::RECORD_EOF;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

RC UpdateOperator::open()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }
  affected_rows_  = 0;
  finished_       = false;
  next_rid_index_ = 0;
  rids_.clear();

  RC rc = children_[0]->open();
  if (OB_FAIL(rc)) {
    return rc;
  }

  while (true) {
    Tuple *child_tuple = nullptr;
    rc                 = children_[0]->next(child_tuple);
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (OB_FAIL(rc)) {
      children_[0]->close();
      return rc;
    }
    if (!child_tuple->has_rid) {
      children_[0]->close();
      LOG_ERROR("the scan tuple carries no rid, cannot update");
      return RC::RECORD_INVALID_RID;
    }
    rids_.push_back(child_tuple->rid);
  }

  RC close_rc = children_[0]->close();
  if (OB_FAIL(close_rc)) {
    return close_rc;
  }
  return RC::SUCCESS;
}

RC UpdateOperator::next(Tuple *&tuple)
{
  if (finished_) {
    return RC::RECORD_EOF;
  }

  const TableMeta &meta = table_->table_meta();

  while (next_rid_index_ < rids_.size()) {
    const RID &rid = rids_[next_rid_index_++];

    /// 读出整条记录，改掉指定的列，再写回去（记录是定长的，索引由 Table 负责维护）
    Record record;
    RC     rc = table_->get_record(rid, record);
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

    rc = table_->update_record(rid, record);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to update record. table=%s, rid=%s, rc=%s",
                table_->table_name().c_str(), rid.to_string().c_str(), strrc(rc));
      return rc;
    }
    affected_rows_ += 1;
  }

  finished_ = true;
  return RC::RECORD_EOF;
}
