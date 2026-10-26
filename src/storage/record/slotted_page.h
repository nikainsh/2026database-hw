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

/**
 * @brief 槽位值的编码
 * @details `>= sizeof(SlottedPageHeader)` 表示记录体在页内的偏移；`< 0` 表示该槽位已删除。
 *          新写入的删除标记是 `-offset`（绝对值 = 记录体原偏移），所以删除后还能**原地复用**
 *          那块记录体空间，不必做页内整理（记录定长、从不搬移）。
 *          老版本把删除记成 `-1`、丢掉了偏移：这种槽位仍被正确识别为「已删除」，
 *          只是没法复用记录体（插入时会重新分配空间）。因此**不需要升级文件 magic**，
 *          旧数据文件照常能打开。
 */
inline bool slot_is_deleted(int32_t slot_value) { return slot_value < 0; }

/// 已删除槽位里是否还留着可复用的记录体偏移（旧格式的 -1 没有）
inline bool slot_has_reusable_offset(int32_t slot_value)
{
  return slot_value <= -static_cast<int32_t>(sizeof(SlottedPageHeader));
}

/// 已删除槽位里保存的「原记录体偏移」
inline int32_t slot_deleted_offset(int32_t slot_value) { return -slot_value; }

/// 新页或整页被删除后的初始状态
inline void init_slotted_page(char *page)
{
  SlottedPageHeader *header = page_header_of(page);
  header->record_count  = 0;
  header->deleted_count = 0;
  header->free_start    = static_cast<int32_t>(sizeof(SlottedPageHeader));
  header->free_end      = BP_PAGE_SIZE;
}

/**
 * @brief 页头自检
 * @details 缓冲池没有数据页 checksum/doublewrite（见 docs/buffer-v3.md），崩溃后可能读到
 *          半写页；各个字段都直接参与偏移计算，任何一个被读坏都可能造成越界写。所以在
 *          使用页之前统一校验一遍：记录区 [sizeof(header), free_start)、空闲区
 *          [free_start, free_end)、槽位目录 [free_end, BP_PAGE_SIZE) 互不重叠且都在页内。
 */
inline bool check_slotted_page(const char *page)
{
  const SlottedPageHeader *header = page_header_of(page);

  if (header->record_count < 0 || header->deleted_count < 0 || header->deleted_count > header->record_count) {
    return false;
  }
  if (header->free_start < static_cast<int32_t>(sizeof(SlottedPageHeader)) || header->free_end > BP_PAGE_SIZE ||
      header->free_start > header->free_end) {
    return false;
  }
  if (header->free_end % static_cast<int32_t>(sizeof(int32_t)) != 0) {
    return false;  /// 槽位目录按 4 字节对齐
  }

  /// 槽位目录占 [free_end, BP_PAGE_SIZE)，每个槽位 4 字节，不能超出这个区间
  const int32_t directory_bytes = BP_PAGE_SIZE - header->free_end;
  if (header->record_count > directory_bytes / static_cast<int32_t>(sizeof(int32_t))) {
    return false;
  }
  return true;
}

/// 找一个已删除的槽位，没有则返回 -1
inline SlotNum find_free_slot(const char *page)
{
  const SlottedPageHeader *header = page_header_of(page);
  /// 没删除过的页直接跳过整个槽位扫描：满页插入是常态，别每次扫 O(槽位数)
  if (header->deleted_count <= 0) {
    return -1;
  }
  for (SlotNum i = 0; i < header->record_count; ++i) {
    if (slot_is_deleted(slot_value_at(page, i))) {
      return i;
    }
  }
  return -1;
}
