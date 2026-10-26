/* database26 buffer manager - global frame pool and replacement */
#include "storage/buffer/frame_manager.h"

#include <chrono>
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
  worker_ = std::thread(&FrameManager::worker_loop, this);
}

void FrameManager::worker_loop()
{
  while (true) {
    PrefetchRequest request;
    bool            has_request = false;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!stop_ && prefetch_queue_.empty()) {
        work_cv_.wait_for(lock, std::chrono::milliseconds(50),
                          [&]() { return stop_ || !prefetch_queue_.empty(); });
      }
      if (stop_) {
        break;
      }
      if (!prefetch_queue_.empty()) {
        request = prefetch_queue_.front();
        prefetch_queue_.pop_front();
        if (closing_owners_.count(request.owner) > 0) {
          prefetch_pending_.erase(page_key(request.owner, request.page));
          continue;
        }
        prefetch_active_.insert(page_key(request.owner, request.page));
        ++owner_active_ops_[request.owner];
        has_request = true;
      }
    }

    if (has_request) {
      FrameId frame_id = INVALID_FRAME_ID;
      RC      rc       = acquire(request.owner, request.page, &frame_id, true);
      if (OB_SUCC(rc)) {
        unpin(frame_id, true);
      }

      std::lock_guard<std::mutex> lock(mutex_);
      const uint64_t key = page_key(request.owner, request.page);
      prefetch_pending_.erase(key);
      prefetch_active_.erase(key);
      auto iter = owner_active_ops_.find(request.owner);
      if (iter != owner_active_ops_.end()) {
        if (--iter->second == 0) {
          owner_active_ops_.erase(iter);
        }
      }
      cv_.notify_all();
      continue;
    }

    /// No prefetch request: use the idle tick to flush a few dirty pages.
    flush_some();
  }
}

void FrameManager::flush_some()
{
  std::vector<FrameId> batch;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (int i = 0; i < total_frames_ && batch.size() < 4; ++i) {
      Frame &frame = frames_[i];
      if (frame.owner() == nullptr || frame.page_id() == INVALID_PAGE_NUM || !frame.is_dirty() ||
          frame.isLoading() || frame.pin_count() > 0 || closing_owners_.count(frame.owner()) > 0) {
        continue;
      }
      frame.increase_pin_count();
      frame.increase_background_pin_count();
      background_pins_.fetch_add(1, std::memory_order_relaxed);
      ++owner_active_ops_[frame.owner()];
      batch.push_back(frame.frame_id());
    }
  }

  for (FrameId frame_id : batch) {
    Frame *frame = frame_at(frame_id);
    if (frame == nullptr) {
      continue;
    }
    DiskBufferPool *owner = frame->owner();

    /// Background flush must not block on a page latch held by a user. If the
    /// latch is busy, skip this page and release the temporary pin immediately.
    std::unique_lock<std::shared_mutex> frame_lock(frame->latch(), std::try_to_lock);
    if (frame_lock.owns_lock()) {
      flush_frame_locked(*frame);
      frame_lock.unlock();
    }
    unpin(frame_id, true);

    std::lock_guard<std::mutex> guard(mutex_);
    auto iter = owner_active_ops_.find(owner);
    if (iter != owner_active_ops_.end()) {
      if (--iter->second == 0) {
        owner_active_ops_.erase(iter);
      }
    }
    cv_.notify_all();
  }
}

void FrameManager::prefetch(DiskBufferPool *owner, PageNum page)
{
  if (owner == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  if (stop_ || closing_owners_.count(owner) > 0) {
    return;
  }
  const uint64_t key = page_key(owner, page);
  if (page_table_.count(key) > 0 || prefetch_pending_.count(key) > 0) {
    return;
  }
  prefetch_pending_.insert(key);
  prefetch_queue_.push_back(PrefetchRequest{owner, page});
  work_cv_.notify_one();
}

void FrameManager::cancel_owner(DiskBufferPool *owner)
{
  if (owner == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  closing_owners_.insert(owner);

  for (auto iter = prefetch_queue_.begin(); iter != prefetch_queue_.end();) {
    if (iter->owner == owner) {
      prefetch_pending_.erase(page_key(owner, iter->page));
      iter = prefetch_queue_.erase(iter);
    } else {
      ++iter;
    }
  }

  auto active = owner_active_ops_.find(owner);
  while (active != owner_active_ops_.end() && active->second > 0) {
    cv_.wait(lock);
    active = owner_active_ops_.find(owner);
  }
}

void FrameManager::cancel_prefetch(DiskBufferPool *owner, PageNum page)
{
  if (owner == nullptr) {
    return;
  }
  const uint64_t key = page_key(owner, page);
  std::unique_lock<std::mutex> lock(mutex_);

  for (auto iter = prefetch_queue_.begin(); iter != prefetch_queue_.end();) {
    if (iter->owner == owner && iter->page == page) {
      prefetch_pending_.erase(key);
      iter = prefetch_queue_.erase(iter);
    } else {
      ++iter;
    }
  }
  while (prefetch_active_.count(key) > 0) {
    cv_.wait(lock);
  }
}

void FrameManager::stop()
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (stop_) {
      return;
    }
    stop_ = true;
  }
  work_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

FrameManager::~FrameManager() { stop(); }

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

RC FrameManager::acquire(DiskBufferPool *owner, PageNum page, FrameId *frame_id, bool background)
{
  if (owner == nullptr || frame_id == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const uint64_t key = page_key(owner, page);

  while (true) {
    auto iter = page_table_.find(key);
    if (iter != page_table_.end()) {
      Frame *frame = frame_at(iter->second);
      if (frame == nullptr) {
        return RC::BUFFERPOOL_CORRUPTED;
      }
      if (frame->isLoading()) {
        cv_.wait(lock, [frame]() { return !frame->isLoading(); });
        continue;
      }
      frame->increase_pin_count();
      if (background) {
        frame->increase_background_pin_count();
        background_pins_.fetch_add(1, std::memory_order_relaxed);
      }
      replacement_.on_access(*frame);
      owner->stats_.hits.fetch_add(1, std::memory_order_relaxed);
      *frame_id = frame->frame_id();
      return RC::SUCCESS;
    }

    const FrameId victim_id = choose_victim_locked(owner);
    if (victim_id == INVALID_FRAME_ID) {
      /// Background pins are temporary. A foreground caller waits for them
      /// instead of reporting a spurious BUFFERPOOL_NOBUF. A background
      /// request never waits (prefetch is best-effort), so it cannot block
      /// stop()/close().
      if (!background && background_pins_.load(std::memory_order_relaxed) > 0) {
        cv_.wait(lock, [this]() { return background_pins_.load(std::memory_order_relaxed) == 0; });
        continue;
      }
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
    frame->set_loading(true);
    if (background) {
      frame->increase_background_pin_count();
      background_pins_.fetch_add(1, std::memory_order_relaxed);
    }
    replacement_.on_access(*frame);
    ++owner_frame_counts_[owner];
    ++used_frames_;
    page_table_[key] = victim_id;

    lock.unlock();
    RC rc = owner->read_page_for_cache(page, frame->data());
    lock.lock();

    frame->set_loading(false);
    if (OB_FAIL(rc)) {
      if (background) {
        frame->decrease_background_pin_count();
        background_pins_.fetch_sub(1, std::memory_order_relaxed);
      }
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
      cv_.notify_all();
      return rc;
    }

    owner->stats_.misses.fetch_add(1, std::memory_order_relaxed);
    owner->stats_.reads.fetch_add(1, std::memory_order_relaxed);
    *frame_id = victim_id;
    cv_.notify_all();
    return RC::SUCCESS;
  }
}
RC FrameManager::remove_page(DiskBufferPool *owner, PageNum page)
{
  if (owner == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const uint64_t key = page_key(owner, page);
  while (true) {
    auto iter = page_table_.find(key);
    if (iter == page_table_.end()) {
      return RC::SUCCESS;
    }

    Frame *frame = frame_at(iter->second);
    if (frame == nullptr) {
      return RC::BUFFERPOOL_CORRUPTED;
    }
    if (frame->pin_count() > 0) {
      /// If the only pins are temporary background pins (prefetch / page
      /// cleaner), wait for them to drain. A real user pin still returns
      /// BUFFERPOOL_PAGE_PINNED immediately.
      if (frame->background_pin_count() > 0 && frame->pin_count() == frame->background_pin_count()) {
        cv_.wait(lock, [frame]() {
          return frame->pin_count() == 0 || frame->background_pin_count() == 0 ||
                 frame->pin_count() != frame->background_pin_count();
        });
        continue;
      }
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
    auto count_iter = owner_frame_counts_.find(owner);
    if (count_iter != owner_frame_counts_.end()) {
      if (--count_iter->second == 0) {
        owner_frame_counts_.erase(count_iter);
      }
    }
    frame.reset();
    frame.set_owner(nullptr);
    --used_frames_;
  }
  owner_frame_counts_.erase(owner);
  closing_owners_.erase(owner);
  return rc;
}

void FrameManager::resume_owner(DiskBufferPool *owner)
{
  if (owner == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  closing_owners_.erase(owner);
}

RC FrameManager::unpin(FrameId frame_id, bool background)
{
  Frame *frame = frame_at(frame_id);
  if (frame == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  if (!background) {
    /// Foreground waiters never wait for a user pin to disappear, so the hot
    /// read path does not need the manager mutex.
    if (frame->pin_count() <= 0) {
      ASSERT(false, "unpin a page which is not pinned. frame=%d", frame_id);
      return RC::INVALID_ARGUMENT;
    }
    frame->decrease_pin_count();
    return RC::SUCCESS;
  }

  /// Background pins are waited on by acquire()/remove_page(). Update them
  /// under the same mutex so a waiter cannot miss the notify between its
  /// condition check and wait.
  std::lock_guard<std::mutex> guard(mutex_);
  if (frame->pin_count() <= 0) {
    ASSERT(false, "unpin a page which is not pinned. frame=%d", frame_id);
    return RC::INVALID_ARGUMENT;
  }
  frame->decrease_background_pin_count();
  background_pins_.fetch_sub(1, std::memory_order_relaxed);
  frame->decrease_pin_count();
  cv_.notify_all();
  return RC::SUCCESS;
}

RC FrameManager::mark_dirty(FrameId frame_id)
{
  Frame *frame = frame_at(frame_id);
  if (frame == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  frame->mark_dirty();
  work_cv_.notify_one();
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
