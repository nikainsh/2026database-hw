/* database26 buffer manager - LRU-K replacement policy */
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.h"

class Frame;

/**
 * @brief LRU-K replacement policy.
 *
 * K=1 behaves like LRU. K=2 protects pages that have been accessed at least
 * twice and evicts one-shot pages first, which makes sequential scans less
 * likely to flush a hot working set.
 */
class LruKPolicy
{
public:
  explicit LruKPolicy(int k = 2);

  /// Must be called under DiskBufferPool::meta_mutex_.
  void on_access(Frame &frame);

  /// Must be called under DiskBufferPool::meta_mutex_.
  FrameId choose_victim(Frame *frames, size_t frame_count) const;

  int k() const { return k_; }

private:
  int      k_       = 2;
  uint64_t counter_ = 0;
};
