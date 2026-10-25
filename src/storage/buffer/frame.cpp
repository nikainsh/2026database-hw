/* database26 buffer manager - page frame and page table */
#include "storage/buffer/frame.h"

void Frame::init(FrameId frame_id)
{
  frame_id_     = frame_id;
  page_id_      = INVALID_PAGE_NUM;
  pin_count_.store(0, std::memory_order_relaxed);
  background_pin_count_.store(0, std::memory_order_relaxed);
  dirty_.store(false, std::memory_order_relaxed);
  loading_      = false;
  owner_        = nullptr;
  access_count_ = 0;
  access_seq_[0] = 0;
  access_seq_[1] = 0;
}

void Frame::reset()
{
  page_id_      = INVALID_PAGE_NUM;
  pin_count_.store(0, std::memory_order_relaxed);
  background_pin_count_.store(0, std::memory_order_relaxed);
  dirty_.store(false, std::memory_order_relaxed);
  loading_      = false;
  access_count_ = 0;
  access_seq_[0] = 0;
  access_seq_[1] = 0;
}

void Frame::record_access(uint64_t sequence)
{
  for (int i = kAccessHistory - 1; i > 0; --i) {
    access_seq_[i] = access_seq_[i - 1];
  }
  access_seq_[0] = sequence;
  if (access_count_ < kAccessHistory) {
    ++access_count_;
  }
}

uint64_t Frame::access_seq(int index) const
{
  if (index < 0 || index >= kAccessHistory) {
    return 0;
  }
  return access_seq_[index];
}

void Frame::reset_access()
{
  access_count_  = 0;
  access_seq_[0] = 0;
  access_seq_[1] = 0;
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
