/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstdint>

#include "common/types.h"
#include "storage/buffer/page.h"

/// 数据页从第 2 页开始（第 0 页属于缓冲池，第 1 页属于表头）
static constexpr PageNum RECORD_FIRST_DATA_PAGE = 2;

/**
 * @brief 数据页的页头（slotted page 布局）
 * @details 槽位数组从页尾向前生长，记录内容从页头向后生长：
 *          slot[i] 保存第 i 条记录的字节偏移，-1 表示这个槽位已被删除。
 */
struct SlottedPageHeader
{
  int32_t record_count;   /// 槽位数量，包含已删除的
  int32_t deleted_count;  /// 已删除的槽位数量
  int32_t free_start;     /// 记录区下一个可用的偏移
  int32_t free_end;       /// 槽位数组的起始偏移
};

inline SlottedPageHeader *page_header_of(char *page)
{
  return reinterpret_cast<SlottedPageHeader *>(page);
}

inline const SlottedPageHeader *page_header_of(const char *page)
{
  return reinterpret_cast<const SlottedPageHeader *>(page);
}

inline int32_t *slot_at(char *page, SlotNum slot)
{
  /// 槽位目录固定从页尾向前生长：第 i 个槽位永远位于 BP_PAGE_SIZE - 4*(i+1)
  return reinterpret_cast<int32_t *>(page + BP_PAGE_SIZE) - 1 - slot;
}

inline int32_t slot_value_at(const char *page, SlotNum slot)
{
  return *slot_at(const_cast<char *>(page), slot);
}

/// 新页或整页被删除后的初始状态
inline void init_slotted_page(char *page)
{
  SlottedPageHeader *header = page_header_of(page);
  header->record_count  = 0;
  header->deleted_count = 0;
  header->free_start    = static_cast<int32_t>(sizeof(SlottedPageHeader));
  header->free_end      = BP_PAGE_SIZE;
}

/// 找一个已删除的槽位，没有则返回 -1
inline SlotNum find_free_slot(const char *page)
{
  const SlottedPageHeader *header = page_header_of(page);
  for (SlotNum i = 0; i < header->record_count; ++i) {
    if (slot_value_at(page, i) == -1) {
      return i;
    }
  }
  return -1;
}
