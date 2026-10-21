/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/buffer/frame.h"

void Frame::init(FrameId frame_id)
{
  frame_id_   = frame_id;
  page_id_    = INVALID_PAGE_NUM;
  pin_count_  = 0;
  dirty_      = false;
  referenced_ = false;
}

void Frame::reset()
{
  page_id_    = INVALID_PAGE_NUM;
  pin_count_  = 0;
  dirty_      = false;
  referenced_ = false;
}

bool PageTable::get(FrameId *frame_id, PageNum page_id) const
{
  auto iter = map_.find(page_id);
  if (iter == map_.end()) {
    return false;
  }
  *frame_id = iter->second;
  return true;
}

bool PageTable::erase(PageNum page_id) { return map_.erase(page_id) > 0; }
