/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstddef>
#include <cstdint>

using PageNum = int32_t;   /// 页编号
using SlotNum = int32_t;   /// 页内槽位编号
using FrameId = int32_t;   /// 缓冲池帧编号

static constexpr PageNum  INVALID_PAGE_NUM  = -1;
static constexpr FrameId  INVALID_FRAME_ID  = -1;
static constexpr SlotNum  INVALID_SLOT_NUM  = -1;

/// 数据文件第 0 页固定为文件头页，第 1 页留给表自身的元数据页
static constexpr PageNum BP_HEADER_PAGE = 0;

enum class ReadWriteMode
{
  READ_ONLY,
  READ_WRITE
};
