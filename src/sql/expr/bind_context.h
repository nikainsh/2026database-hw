/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "common/rc.h"
#include "storage/table/table_meta.h"

/**
 * @brief 名称解析用的上下文：当前查询涉及哪些表
 * @details 对应 miniob 里 ExpressionBinder 依赖的 meta 信息集合。
 *          table_index 就是表在 from 列表里的下标，元组里每一列都带着它。
 */
class BindContext
{
public:
  struct TableRef
  {
    std::string        table_name;  /// 用户写的名字（可能是别名）
    std::string        real_name;   /// 真实的表名
    const TableMeta   *meta       = nullptr;
    int                table_index = 0;
  };

  void add_table(const std::string &name, const std::string &real_name, const TableMeta *meta)
  {
    tables_.push_back(TableRef{name, real_name, meta, static_cast<int>(tables_.size())});
  }

  const std::vector<TableRef> &tables() const { return tables_; }
  int                          table_count() const { return static_cast<int>(tables_.size()); }

  /// 限名可以为空：只有一张表时直接命中，否则返回 nullptr
  const TableRef *find_table(const std::string &name) const
  {
    if (name.empty()) {
      return tables_.size() == 1 ? &tables_.front() : nullptr;
    }
    for (const TableRef &table : tables_) {
      if (table.table_name == name || table.real_name == name) {
        return &table;
      }
    }
    return nullptr;
  }

private:
  std::vector<TableRef> tables_;
};
