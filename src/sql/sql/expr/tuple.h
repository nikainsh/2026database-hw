/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "storage/record/record.h"

/// 元组里一列的来源：哪张表的哪一列；table_index 为 -1 表示计算出来的列
struct ColumnRef
{
  int table_index  = -1;
  int column_index = -1;

  ColumnRef() = default;
  ColumnRef(int table, int column) : table_index(table), column_index(column) {}

  bool operator==(const ColumnRef &other) const
  {
    return table_index == other.table_index && column_index == other.column_index;
  }
};

/**
 * @brief 执行引擎里流动的一行数据
 * @details miniob 用 CompositeTuple 来表示 join 之后的宽元组并计算 slot 偏移；
 *          这里改成「每一列都自带来源」的扁平结构：join 时直接把右边的列接在后面，
 *          表达式求值按 (table_index, column_index) 找列，避免偏移计算出错。
 *          输出列名由语句的 select 列表决定，元组本身只带值和来源。
 */
class Tuple
{
public:
  std::vector<Value>      values;
  std::vector<ColumnRef>  refs;  /// 与 values 平行

  RID  rid;
  bool has_rid = false;

  void append(const Value &value, const ColumnRef &ref)
  {
    values.push_back(value);
    refs.push_back(ref);
  }

  void append_all(const Tuple &other)
  {
    values.insert(values.end(), other.values.begin(), other.values.end());
    refs.insert(refs.end(), other.refs.begin(), other.refs.end());
  }

  /// 找不到返回 -1
  int find(const ColumnRef &ref) const
  {
    for (size_t i = 0; i < refs.size(); ++i) {
      if (refs[i] == ref) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  bool get(const ColumnRef &ref, Value &value) const
  {
    const int index = find(ref);
    if (index < 0) {
      return false;
    }
    value = values[index];
    return true;
  }

  void clear()
  {
    values.clear();
    refs.clear();
    has_rid = false;
  }

  size_t column_count() const { return values.size(); }

  std::string to_string() const;
};
