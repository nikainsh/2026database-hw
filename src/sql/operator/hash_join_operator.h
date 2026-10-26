#pragma once

#include <unordered_map>

#include "sql/expr/expression.h"
#include "sql/plan/physical_operator.h"

/**
 * In-memory, single-key inner equijoin. Child 0 is built once, child 1 probed once.
 * Keys must both have key_type (INT/BIGINT/CHARS); no casts, NULL, float or spill.
 * residual is borrowed and must outlive execution. Output is left then right.
 * max_build_bytes limits an estimate of retained build storage, not process RSS.
 * A failed child open owns its own partial cleanup; successfully opened children
 * are closed here on failure. After any failure, close/reopen before retrying.
 */
class HashJoinOperator : public PhysicalOperator
{
public:
  static constexpr size_t DEFAULT_MAX_BUILD_BYTES = 8 * 1024 * 1024;
  HashJoinOperator(ColumnRef left_key, ColumnRef right_key, AttrType key_type,
                   Expression *residual = nullptr, size_t max_build_bytes = DEFAULT_MAX_BUILD_BYTES)
      : left_key_(left_key), right_key_(right_key), key_type_(key_type),
        residual_(residual), max_build_bytes_(max_build_bytes) {}

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::HASH_JOIN; }
  RC open() override;
  RC next(Tuple *&tuple) override;
  RC close() override;
  std::string describe() const override;

private:
  using Table = std::unordered_map<std::string, std::vector<Tuple>>;
  RC key_of(const Tuple &tuple, ColumnRef ref, std::string &key) const;
  bool charge_row(const Tuple &tuple, const std::string &key);
  RC fail(RC error);

  ColumnRef left_key_, right_key_;
  AttrType key_type_;
  Expression *residual_;
  size_t max_build_bytes_, estimated_build_bytes_ = 0;
  Table table_;
  const std::vector<Tuple> *matches_ = nullptr;
  size_t match_index_ = 0;
  Tuple probe_, joined_;
  bool left_opened_ = false, right_opened_ = false, active_ = false, eof_ = false;
};
