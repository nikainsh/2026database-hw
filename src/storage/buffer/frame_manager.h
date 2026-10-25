/* database26 buffer manager - global frame pool and replacement */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/rc.h"
#include "common/types.h"
#include "storage/buffer/frame.h"
#include "storage/buffer/replacement.h"

class DiskBufferPool;

/**
 * @brief Global frame pool shared by all open buffer pools.
 *
 * Phase 1: correctness first. A page miss keeps the manager mutex while reading
 * the page from disk, so different files do not fault in parallel yet. The
 * global page table and LRU-K victim selection are already global, which is the
 * important step beyond v2's per-file frame arrays.
 */
class FrameManager
{
public:
  explicit FrameManager(int total_frames);
  ~FrameManager();

  FrameManager(const FrameManager &)            = delete;
  FrameManager &operator=(const FrameManager &) = delete;

  RC acquire(DiskBufferPool *owner, PageNum page, FrameId *frame_id, bool background = false);
  /// Queue a non-blocking prefetch. Duplicate/queued/cached pages are ignored.
  void prefetch(DiskBufferPool *owner, PageNum page);
  /// Stop accepting prefetches for `owner` and wait for in-flight prefetches.
  void cancel_owner(DiskBufferPool *owner);
  /// Cancel/await an in-flight prefetch for one page before dispose/reuse.
  void cancel_prefetch(DiskBufferPool *owner, PageNum page);
  /// Stop the background worker. Idempotent.
  void stop();
  RC remove_page(DiskBufferPool *owner, PageNum page);
  RC flush_page(DiskBufferPool *owner, PageNum page);
  RC flush_owner(DiskBufferPool *owner);
  RC remove_owner(DiskBufferPool *owner);
  void resume_owner(DiskBufferPool *owner);
  bool has_dirty(DiskBufferPool *owner) const;

  RC   unpin(FrameId frame_id, bool background = false);
  RC   mark_dirty(FrameId frame_id);
  char *data(FrameId frame_id);

  void lock_shared(FrameId frame_id);
  void lock_exclusive(FrameId frame_id);
  void unlock_shared(FrameId frame_id);
  void unlock_exclusive(FrameId frame_id);

  int used_frames() const;
  int total_frames() const { return total_frames_; }

private:
  struct PrefetchRequest
  {
    DiskBufferPool *owner = nullptr;
    PageNum         page  = INVALID_PAGE_NUM;
  };

  static uint64_t page_key(DiskBufferPool *owner, PageNum page);
  Frame          *frame_at(FrameId frame_id) const;
  void            worker_loop();
  void            flush_some();
  FrameId         choose_victim_locked(DiskBufferPool *owner);
  RC              flush_frame_locked(Frame &frame);

private:
  std::unique_ptr<Frame[]> frames_;
  std::unordered_map<uint64_t, FrameId> page_table_;
  std::unordered_map<DiskBufferPool *, int> owner_frame_counts_;
  LruKPolicy replacement_{2};

  mutable std::mutex      mutex_;
  std::condition_variable cv_;
  std::condition_variable work_cv_;
  int total_frames_ = 0;
  int used_frames_  = 0;

  std::deque<PrefetchRequest>        prefetch_queue_;
  std::unordered_set<uint64_t>       prefetch_pending_;
  std::unordered_set<uint64_t>       prefetch_active_;
  std::unordered_map<DiskBufferPool *, int> owner_active_ops_;
  std::unordered_set<DiskBufferPool *>      closing_owners_;
  std::atomic<int>                    background_pins_{0};
  std::thread                        worker_;
  bool                               stop_ = false;
};
