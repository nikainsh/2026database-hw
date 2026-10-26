/* database26 buffer manager - LRU-K replacement policy */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

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

  /// Must be called under FrameManager::mutex_.
  void on_access(Frame &frame);

  /// Must be called under the frame manager lock.
  FrameId choose_victim(Frame *frames, size_t frame_count) const;

  /// Same, but only considers frames accepted by `allow`.
  FrameId choose_victim(Frame *frames, size_t frame_count,
                        const std::function<bool(const Frame &)> &allow) const;

  int k() const { return k_; }

private:
  int      k_       = 2;
  uint64_t counter_ = 0;
};
