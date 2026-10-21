/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "common/rc.h"
#include "common/types.h"

/// 一条记录在文件中的位置：页号 + 页内槽位号
struct RID
{
  PageNum page_num = INVALID_PAGE_NUM;
  SlotNum slot_num = INVALID_SLOT_NUM;

  RID() = default;
  RID(PageNum page, SlotNum slot) : page_num(page), slot_num(slot) {}

  bool operator==(const RID &other) const { return page_num == other.page_num && slot_num == other.slot_num; }
  bool operator!=(const RID &other) const { return !(*this == other); }

  /// 用于 B+Tree 中作为索引值时的比较（先页号后槽位号）
  int compare(const RID &other) const
  {
    if (page_num != other.page_num) {
      return page_num < other.page_num ? -1 : 1;
    }
    if (slot_num != other.slot_num) {
      return slot_num < other.slot_num ? -1 : 1;
    }
    return 0;
  }

  std::string to_string() const
  {
    return "(" + std::to_string(page_num) + "," + std::to_string(slot_num) + ")";
  }
};

/**
 * @brief 一条记录
 * @details 与 miniob 一样，记录可以只引用页面上的内存（零拷贝，要求上层还 pin 着页），
 *          也可以通过 copy_data() 持有一份自己的副本。
 */
class Record
{
public:
  Record() = default;

  /// 引用外部内存（通常是缓冲池帧里的页），不拥有它
  void set_data(char *data, int len)
  {
    data_ = data;
    len_  = len;
  }

  /// 持有一份副本，离开页的 pin 范围后仍然有效
  RC copy_data(const char *data, int len)
  {
    if (len <= 0) {
      return RC::INVALID_ARGUMENT;
    }
    owner_.assign(data, data + len);
    data_ = nullptr;
    len_  = 0;
    return RC::SUCCESS;
  }

  char *data() { return owner_.empty() ? data_ : owner_.data(); }
  const char *data() const { return owner_.empty() ? data_ : owner_.data(); }
  int len() const { return owner_.empty() ? len_ : static_cast<int>(owner_.size()); }

  const RID &rid() const { return rid_; }
  RID       &rid() { return rid_; }
  void       set_rid(const RID &rid) { rid_ = rid; }
  void       set_rid(PageNum page_num, SlotNum slot_num)
  {
    rid_.page_num = page_num;
    rid_.slot_num = slot_num;
  }

private:
  RID              rid_;
  char            *data_ = nullptr;
  int              len_  = 0;
  std::vector<char> owner_;  /// 非空表示这条记录自己管理内存
};
