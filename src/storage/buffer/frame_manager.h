/* database26 buffer manager - global frame pool and replacement */
#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
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
  ~FrameManager() = default;

  FrameManager(const FrameManager &)            = delete;
  FrameManager &operator=(const FrameManager &) = delete;

  RC acquire(DiskBufferPool *owner, PageNum page, FrameId *frame_id);
  RC remove_page(DiskBufferPool *owner, PageNum page);
  RC flush_page(DiskBufferPool *owner, PageNum page);
  RC flush_owner(DiskBufferPool *owner);
  RC remove_owner(DiskBufferPool *owner);
  bool has_dirty(DiskBufferPool *owner) const;

  RC   unpin(FrameId frame_id);
  RC   mark_dirty(FrameId frame_id);
  char *data(FrameId frame_id);

  void lock_shared(FrameId frame_id);
  void lock_exclusive(FrameId frame_id);
  void unlock_shared(FrameId frame_id);
  void unlock_exclusive(FrameId frame_id);

  int used_frames() const;
  int total_frames() const { return total_frames_; }

private:
  static uint64_t page_key(DiskBufferPool *owner, PageNum page);
  Frame          *frame_at(FrameId frame_id) const;
  FrameId         choose_victim_locked(DiskBufferPool *owner);
  RC              flush_frame_locked(Frame &frame);

private:
  std::unique_ptr<Frame[]> frames_;
  std::unordered_map<uint64_t, FrameId> page_table_;
  std::unordered_map<DiskBufferPool *, int> owner_frame_counts_;
  LruKPolicy replacement_{2};

  mutable std::mutex mutex_;
  int total_frames_ = 0;
  int used_frames_  = 0;
};
