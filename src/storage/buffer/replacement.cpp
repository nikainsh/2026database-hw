/* database26 buffer manager - LRU-K replacement policy */
#include "storage/buffer/replacement.h"

#include <algorithm>
#include <limits>

#include "storage/buffer/frame.h"

LruKPolicy::LruKPolicy(int k) : k_(std::max(1, std::min(k, Frame::kAccessHistory))) {}

void LruKPolicy::on_access(Frame &frame)
{
  frame.record_access(++counter_);
}

FrameId LruKPolicy::choose_victim(Frame *frames, size_t frame_count) const
{
  FrameId victim        = INVALID_FRAME_ID;
  uint64_t victim_score = 0;

  for (size_t i = 0; i < frame_count; ++i) {
    Frame &frame = frames[i];
    if (frame.pin_count() > 0) {
      continue;
    }

    /// Prefer a completely empty frame.
    if (frame.page_id() == INVALID_PAGE_NUM) {
      return frame.frame_id();
    }

    uint64_t distance = std::numeric_limits<uint64_t>::max();
    if (frame.access_count() >= k_) {
      const uint64_t kth = frame.access_seq(k_ - 1);
      distance          = counter_ >= kth ? counter_ - kth : 0;
    }

    if (victim == INVALID_FRAME_ID || distance > victim_score) {
      victim       = frame.frame_id();
      victim_score = distance;
    }
  }

  return victim;
}
