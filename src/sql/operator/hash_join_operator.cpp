#include "sql/operator/hash_join_operator.h"

#include <new>

RC HashJoinOperator::key_of(const Tuple &tuple, bool build, std::string &key) const
{
  key.clear();
  for (const HashJoinKey &part : keys_) {
    Value value;
    if (!tuple.get(build ? part.left : part.right, value)) return RC::SCHEMA_FIELD_NOT_EXIST;
    if (value.type() != part.type) return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    const uint64_t length = part.type == AttrType::CHARS ? value.get_string().size()
                                                       : static_cast<uint64_t>(attr_type_fixed_len(part.type));
    key.push_back(static_cast<char>(part.type));
    for (size_t i = 0; i < sizeof(uint64_t); ++i) key.push_back(static_cast<char>((length >> (i * 8)) & 0xff));
    if (part.type == AttrType::CHARS) {
      key.append(value.get_string());
    } else {
      char bytes[sizeof(int64_t)];
      RC rc = value.pack_into(bytes, static_cast<int>(length));
      if (OB_FAIL(rc)) return rc;
      key.append(bytes, static_cast<size_t>(length));
    }
  }
  return RC::SUCCESS;
}

bool HashJoinOperator::charge_row(const Tuple &tuple, const std::string &key)
{
  // Charge per row (including duplicate keys), with an allowance for map/vector
  // overhead. Actual buckets, capacity growth, and allocator overhead may differ.
  size_t bytes = estimated_build_bytes_;
  auto add = [&](size_t n) {
    if (bytes > max_build_bytes_ || n > max_build_bytes_ - bytes) return false;
    bytes += n;
    return true;
  };
  if (!add(sizeof(Tuple)) || !add(sizeof(std::string)) || !add(sizeof(std::vector<Tuple>)) ||
      !add(4 * sizeof(void *)) || !add(key.size())) return false;
  for (const Value &value : tuple.values) {
    if (!add(sizeof(Value))) return false;
    if (value.type() == AttrType::CHARS && !add(value.get_string().size())) return false;
  }
  for (size_t i = 0; i < tuple.refs.size(); ++i) if (!add(sizeof(ColumnRef))) return false;
  estimated_build_bytes_ = bytes;
  return true;
}

RC HashJoinOperator::fail(RC error)
{
  close(); // Preserve the original error even if cleanup also reports failure.
  return error;
}

RC HashJoinOperator::open()
{
  if (active_ || left_opened_ || right_opened_) return RC::INVALID_ARGUMENT;
  if (children_.size() != 2) return RC::INTERNAL;
  if (keys_.empty()) return RC::INVALID_ARGUMENT;
  for (const HashJoinKey &part : keys_) {
    if (part.left.table_index < 0 || part.left.column_index < 0 ||
        part.right.table_index < 0 || part.right.column_index < 0) return RC::INVALID_ARGUMENT;
    if (part.type != AttrType::INT && part.type != AttrType::BIGINT && part.type != AttrType::CHARS) return RC::UNSUPPORTED;
  }
  estimated_build_bytes_ = 0;
  match_index_ = 0;
  eof_ = false;
  affected_rows_ = 0;
  try {
    RC rc = children_[0]->open_tracked();
    if (OB_FAIL(rc)) return rc;
    left_opened_ = true;
    while (true) {
      Tuple *row = nullptr;
      rc = children_[0]->next_tracked(row);
      if (rc == RC::RECORD_EOF) break;
      if (OB_FAIL(rc)) return fail(rc);
      if (row == nullptr) return fail(RC::INTERNAL);
      std::string key;
      rc = key_of(*row, true, key);
      if (OB_FAIL(rc)) return fail(rc);
      if (!charge_row(*row, key)) return fail(RC::NOMEM);
      table_[key].push_back(*row); // Own a copy: input scans reuse their tuples.
      count_build_row(estimated_build_bytes_);
    }
    rc = children_[0]->close_tracked();
    left_opened_ = false;
    if (OB_FAIL(rc)) return fail(rc);
    active_ = true;
    if (table_.empty()) {
      eof_ = true; // The probe side is never opened when no result is possible.
      return RC::SUCCESS;
    }
    rc = children_[1]->open_tracked();
    if (OB_FAIL(rc)) return fail(rc);
    right_opened_ = true;
    return RC::SUCCESS;
  } catch (const std::bad_alloc &) {
    return fail(RC::NOMEM);
  }
}

RC HashJoinOperator::next(Tuple *&tuple)
{
  if (!active_) return RC::INTERNAL;
  if (eof_) return RC::RECORD_EOF;
  try {
    while (true) {
      while (matches_ != nullptr && match_index_ < matches_->size()) {
        const Tuple &left = (*matches_)[match_index_++];
        count_candidate_pair();
        joined_.clear();
        joined_.append_all(left);
        joined_.append_all(probe_);
        joined_.rid = left.rid;
        joined_.has_rid = left.has_rid;
        if (residual_ != nullptr) {
          Value value;
          RC rc = residual_->evaluate(joined_, value);
          if (OB_FAIL(rc)) return fail(rc);
          if (value.type() != AttrType::BOOLEAN) return fail(RC::SCHEMA_FIELD_TYPE_MISMATCH);
          if (!value.get_bool()) continue;
        }
        tuple = &joined_;
        return RC::SUCCESS;
      }
      matches_ = nullptr;
      match_index_ = 0;
      Tuple *row = nullptr;
      RC rc = children_[1]->next_tracked(row);
      if (rc == RC::RECORD_EOF) { eof_ = true; return rc; }
      if (OB_FAIL(rc)) return fail(rc);
      if (row == nullptr) return fail(RC::INTERNAL);
      count_probe_row();
      std::string key;
      rc = key_of(*row, false, key);
      if (OB_FAIL(rc)) return fail(rc);
      auto iter = table_.find(key);
      if (iter == table_.end()) continue;
      probe_ = *row; // Retained across next calls when this key matches many rows.
      matches_ = &iter->second;
    }
  } catch (const std::bad_alloc &) {
    return fail(RC::NOMEM);
  }
}

RC HashJoinOperator::close()
{
  RC rc = RC::SUCCESS;
  if (right_opened_) {
    rc = children_[1]->close_tracked();
    right_opened_ = false;
  }
  if (left_opened_) {
    RC one = children_[0]->close_tracked();
    left_opened_ = false;
    if (OB_SUCC(rc)) rc = one;
  }
  active_ = false;
  eof_ = false;
  matches_ = nullptr;
  match_index_ = 0;
  estimated_build_bytes_ = 0;
  Table{}.swap(table_);
  probe_ = Tuple{};
  joined_ = Tuple{};
  return rc;
}

std::string HashJoinOperator::describe() const
{
  std::string text = "hash_join(build=left, key_count=" + std::to_string(keys_.size()) + ", keys=[";
  for (size_t i = 0; i < keys_.size(); ++i) {
    const auto &part = keys_[i];
    if (i) text += ", ";
    text += std::to_string(part.left.table_index) + "." + std::to_string(part.left.column_index) + "=" +
            std::to_string(part.right.table_index) + "." + std::to_string(part.right.column_index) +
            ":" + attr_type_to_string(part.type);
  }
  return text + "], budget=" + std::to_string(max_build_bytes_) + ")";
}
