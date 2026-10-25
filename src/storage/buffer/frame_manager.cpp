/* database26 buffer manager - global frame pool and replacement */
#include "storage/buffer/frame_manager.h"

#include <limits>

#include "common/log.h"
#include "storage/buffer/disk_buffer_pool.h"

FrameManager::FrameManager(int total_frames) : total_frames_(total_frames > 0 ? total_frames : 1)
{
  frames_ = std::make_unique<Frame[]>(total_frames_);
  for (int i = 0; i < total_frames_; ++i) {
    frames_[i].init(static_cast<FrameId>(i));
    frames_[i].set_owner(nullptr);
  }
}

uint64_t FrameManager::page_key(DiskBufferPool *owner, PageNum page)
{
  return (static_cast<uint64_t>(owner->pool_id()) << 32) | static_cast<uint32_t>(page);
}

Frame *FrameManager::frame_at(FrameId frame_id) const
{
  if (frame_id < 0 || frame_id >= total_frames_ || !frames_) {
    return nullptr;
  }
  return &frames_[frame_id];
}

FrameId FrameManager::choose_victim_locked(DiskBufferPool *owner)
{
  auto iter = owner_frame_counts_.find(owner);
  const int owner_count = iter == owner_frame_counts_.end() ? 0 : iter->second;
  const bool quota_full = owner_count >= owner->frame_limit();

  auto allow = [&](const Frame &frame) {
    if (quota_full) {
      return frame.owner() == owner;
    }
    return true;
  };
  return replacement_.choose_victim(frames_.get(), static_cast<size_t>(total_frames_), allow);
}

RC FrameManager::flush_frame_locked(Frame &frame)
{
  if (!frame.is_dirty()) {
    return RC::SUCCESS;
  }
  RC rc = frame.owner()->flush_frame_data(frame.page_id(), frame.data());
  if (OB_SUCC(rc)) {
    frame.clear_dirty();
  }
  return rc;
}

RC FrameManager::acquire(DiskBufferPool *owner, PageNum page, FrameId *frame_id)
{
  if (owner == nullptr || frame_id == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> guard(mutex_);
  const uint64_t key = page_key(owner, page);
  auto           iter = page_table_.find(key);
  if (iter != page_table_.end()) {
    Frame *frame = frame_at(iter->second);
    if (frame == nullptr) {
      return RC::BUFFERPOOL_CORRUPTED;
    }
    frame->increase_pin_count();
    replacement_.on_access(*frame);
    owner->stats_.hits.fetch_add(1, std::memory_order_relaxed);
    *frame_id = frame->frame_id();
    return RC::SUCCESS;
  }

  const FrameId victim_id = choose_victim_locked(owner);
  if (victim_id == INVALID_FRAME_ID) {
    owner->stats_.no_buffer.fetch_add(1, std::memory_order_relaxed);
    return RC::BUFFERPOOL_NOBUF;
  }

  Frame *frame = frame_at(victim_id);
  if (frame == nullptr) {
    return RC::BUFFERPOOL_CORRUPTED;
  }

  if (frame->page_id() != INVALID_PAGE_NUM) {
    if (frame->is_dirty()) {
      RC rc = flush_frame_locked(*frame);
      if (OB_FAIL(rc)) {
        return rc;
      }
      frame->owner()->stats_.dirty_evictions.fetch_add(1, std::memory_order_relaxed);
    }
    page_table_.erase(page_key(frame->owner(), frame->page_id()));
    auto count_iter = owner_frame_counts_.find(frame->owner());
    if (count_iter != owner_frame_counts_.end()) {
      if (--count_iter->second == 0) {
        owner_frame_counts_.erase(count_iter);
      }
    }
    frame->owner()->stats_.evictions.fetch_add(1, std::memory_order_relaxed);
    frame->reset();
    frame->set_owner(nullptr);
    --used_frames_;
  }

  frame->set_owner(owner);
  frame->set_page_id(page);
  frame->set_pin_count(1);
  frame->clear_dirty();
  replacement_.on_access(*frame);
  ++owner_frame_counts_[owner];
  ++used_frames_;
  page_table_[key] = victim_id;

  RC rc = owner->read_page_for_cache(page, frame->data());
  if (OB_FAIL(rc)) {
    page_table_.erase(key);
    auto count_iter = owner_frame_counts_.find(owner);
    if (count_iter != owner_frame_counts_.end()) {
      if (--count_iter->second == 0) {
        owner_frame_counts_.erase(count_iter);
      }
    }
    --used_frames_;
    frame->reset();
    frame->set_owner(nullptr);
    return rc;
  }

  owner->stats_.misses.fetch_add(1, std::memory_order_relaxed);
  owner->stats_.reads.fetch_add(1, std::memory_order_relaxed);
  *frame_id = victim_id;
  return RC::SUCCESS;
}

RC FrameManager::remove_page(DiskBufferPool *owner, PageNum page)
{
  if (owner == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> guard(mutex_);
  const uint64_t key = page_key(owner, page);
  auto           iter = page_table_.find(key);
  if (iter == page_table_.end()) {
    return RC::SUCCESS;
  }

  Frame *frame = frame_at(iter->second);
  if (frame == nullptr) {
    return RC::BUFFERPOOL_CORRUPTED;
  }
  if (frame->pin_count() > 0) {
    return RC::BUFFERPOOL_PAGE_PINNED;
  }

  page_table_.erase(iter);
  auto count_iter = owner_frame_counts_.find(owner);
  if (count_iter != owner_frame_counts_.end()) {
    if (--count_iter->second == 0) {
      owner_frame_counts_.erase(count_iter);
    }
  }
  frame->reset();
  frame->set_owner(nullptr);
  --used_frames_;
  return RC::SUCCESS;
}

RC FrameManager::flush_page(DiskBufferPool *owner, PageNum page)
{
  if (owner == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  FrameId frame_id = INVALID_FRAME_ID;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto iter = page_table_.find(page_key(owner, page));
    if (iter == page_table_.end()) {
      return RC::SUCCESS;
    }
    frame_id = iter->second;
    Frame *frame = frame_at(frame_id);
    if (frame == nullptr) {
      return RC::BUFFERPOOL_CORRUPTED;
    }
    frame->increase_pin_count();
  }

  Frame *frame = frame_at(frame_id);
  lock_exclusive(frame_id);
  RC rc = flush_frame_locked(*frame);
  unlock_exclusive(frame_id);
  unpin(frame_id);
  return rc;
}

RC FrameManager::flush_owner(DiskBufferPool *owner)
{
  if (owner == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::vector<FrameId> dirty;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (int i = 0; i < total_frames_; ++i) {
      Frame &frame = frames_[i];
      if (frame.owner() == owner && frame.page_id() != INVALID_PAGE_NUM && frame.is_dirty()) {
        frame.increase_pin_count();
        dirty.push_back(frame.frame_id());
      }
    }
  }

  RC rc = RC::SUCCESS;
  for (FrameId frame_id : dirty) {
    Frame *frame = frame_at(frame_id);
    if (frame == nullptr) {
      rc = RC::BUFFERPOOL_CORRUPTED;
      continue;
    }
    lock_exclusive(frame_id);
    RC one = flush_frame_locked(*frame);
    unlock_exclusive(frame_id);
    unpin(frame_id);
    if (OB_FAIL(one) && OB_SUCC(rc)) {
      rc = one;
    }
  }
  return rc;
}

bool FrameManager::has_dirty(DiskBufferPool *owner) const
{
  std::lock_guard<std::mutex> guard(mutex_);
  for (int i = 0; i < total_frames_; ++i) {
    const Frame &frame = frames_[i];
    if (frame.owner() == owner && frame.page_id() != INVALID_PAGE_NUM && frame.is_dirty()) {
      return true;
    }
  }
  return false;
}

RC FrameManager::remove_owner(DiskBufferPool *owner)
{
  if (owner == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> guard(mutex_);
  RC rc = RC::SUCCESS;
  for (int i = 0; i < total_frames_; ++i) {
    Frame &frame = frames_[i];
    if (frame.owner() != owner) {
      continue;
    }
    if (frame.pin_count() > 0) {
      return RC::BUFFERPOOL_HANDLE_BUSY;
    }
    if (frame.is_dirty()) {
      RC one = flush_frame_locked(frame);
      if (OB_FAIL(one)) {
        return one;
      }
    }
    page_table_.erase(page_key(owner, frame.page_id()));
    frame.reset();
    frame.set_owner(nullptr);
    --used_frames_;
  }
  owner_frame_counts_.erase(owner);
  return rc;
}

RC FrameManager::unpin(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  if (frame == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  const int old = frame->pin_count();
  if (old <= 0) {
    ASSERT(false, "unpin a page which is not pinned. frame=%d", frame_id);
    return RC::INVALID_ARGUMENT;
  }
  frame->decrease_pin_count();
  return RC::SUCCESS;
}

RC FrameManager::mark_dirty(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  if (frame == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  frame->mark_dirty();
  return RC::SUCCESS;
}

char *FrameManager::data(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  ASSERT(frame != nullptr, "invalid frame id. frame=%d", frame_id);
  return frame->data();
}

void FrameManager::lock_shared(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  ASSERT(frame != nullptr, "invalid frame id. frame=%d", frame_id);
  frame->latch().lock_shared();
}

void FrameManager::lock_exclusive(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  ASSERT(frame != nullptr, "invalid frame id. frame=%d", frame_id);
  frame->latch().lock();
}

void FrameManager::unlock_shared(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  if (frame != nullptr) {
    frame->latch().unlock_shared();
  }
}

void FrameManager::unlock_exclusive(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  if (frame != nullptr) {
    frame->latch().unlock();
  }
}

int FrameManager::used_frames() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return used_frames_;
}
