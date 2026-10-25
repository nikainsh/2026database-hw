/* database26 buffer manager - page frame and page table */
#pragma once

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "common/types.h"
#include "storage/buffer/page.h"

class DiskBufferPool;

/**
 * @brief One in-memory page frame.
 *
 * Thread-safety contract:
 *   - pin_count_ and dirty_ are atomic.
 *   - page_id_, owner_, access history are protected by DiskBufferPool::meta_mutex_.
 *   - latch_ protects the 8KB page bytes. Readers take a shared lock, writers an
 *     exclusive lock. A PageHandle holds the latch until reset/destruction.
 */
class Frame
{
public:
  static constexpr int kAccessHistory = 2;

  Frame() = default;
  explicit Frame(FrameId frame_id) : frame_id_(frame_id) {}

  void init(FrameId frame_id);
  void reset();

  FrameId frame_id() const { return frame_id_; }
  PageNum page_id() const { return page_id_; }
  void    set_page_id(PageNum page_id) { page_id_ = page_id; }

  int  pin_count() const { return pin_count_.load(std::memory_order_relaxed); }
  void increase_pin_count() { pin_count_.fetch_add(1, std::memory_order_relaxed); }
  void decrease_pin_count() { pin_count_.fetch_sub(1, std::memory_order_relaxed); }
  void set_pin_count(int value) { pin_count_.store(value, std::memory_order_relaxed); }

  bool is_dirty() const { return dirty_.load(std::memory_order_relaxed); }
  void mark_dirty() { dirty_.store(true, std::memory_order_relaxed); }
  void clear_dirty() { dirty_.store(false, std::memory_order_relaxed); }

  /// Protected by FrameManager::mutex_.
  bool isLoading() const { return loading_; }
  void set_loading(bool value) { loading_ = value; }

  DiskBufferPool *owner() const { return owner_; }
  void            set_owner(DiskBufferPool *owner) { owner_ = owner; }

  char       *data() { return page_.data; }
  const char *data() const { return page_.data; }

  std::shared_mutex &latch() { return latch_; }

  /// Access history is only touched under DiskBufferPool::meta_mutex_.
  void record_access(uint64_t sequence);
  int  access_count() const { return access_count_; }
  uint64_t access_seq(int index) const;
  void reset_access();

private:
  FrameId         frame_id_     = INVALID_FRAME_ID;
  PageNum         page_id_      = INVALID_PAGE_NUM;
  std::atomic<int> pin_count_{0};
  std::atomic<bool> dirty_{false};
  bool             loading_     = false;
  DiskBufferPool *owner_        = nullptr;
  Page            page_;
  std::shared_mutex latch_;

  uint64_t access_seq_[kAccessHistory] = {0, 0};
  int      access_count_               = 0;
};

/// page_num -> frame_id map. It is not internally synchronized; DiskBufferPool
/// protects it with meta_mutex_.
class PageTable
{
public:
  void insert(PageNum page_id, FrameId frame_id) { map_[page_id] = frame_id; }
  bool get(FrameId *frame_id, PageNum page_id) const;
  bool erase(PageNum page_id);
  bool contains(PageNum page_id) const { return map_.count(page_id) > 0; }
  void clear() { map_.clear(); }
  void reserve(size_t n) { map_.reserve(n); }
  size_t size() const { return map_.size(); }

private:
  std::unordered_map<PageNum, FrameId> map_;
};
