/* database26 buffer manager - LRU-K replacement policy unit test */
#include <cassert>
#include <cstdio>
#include <memory>

#include "storage/buffer/frame.h"
#include "storage/buffer/replacement.h"

static std::unique_ptr<Frame[]> make_frames(int n)
{
  auto frames = std::make_unique<Frame[]>(n);
  for (int i = 0; i < n; ++i) {
    frames[i].init(static_cast<FrameId>(i));
    frames[i].set_page_id(i + 1);
  }
  return frames;
}

int main()
{
  {
    auto frames = make_frames(4);
    LruKPolicy policy(2);

    for (int round = 0; round < 2; ++round) {
      for (int i = 0; i < 4; ++i) {
        policy.on_access(frames[i]);
      }
    }

    /// frame 0 has the oldest second access.
    assert(policy.choose_victim(frames.get(), 4) == 0);

    frames[0].increase_pin_count();
    assert(policy.choose_victim(frames.get(), 4) == 1);
    frames[0].decrease_pin_count();
  }

  {
    auto frames = make_frames(3);
    LruKPolicy policy(2);

    /// frame 1 and 2 are hot; frame 0 has only one access.
    policy.on_access(frames[1]);
    policy.on_access(frames[1]);
    policy.on_access(frames[2]);
    policy.on_access(frames[2]);
    policy.on_access(frames[0]);

    assert(policy.choose_victim(frames.get(), 3) == 0);
  }

  {
    auto frames = std::make_unique<Frame[]>(3);
    for (int i = 0; i < 3; ++i) {
      frames[i].init(static_cast<FrameId>(i));
    }
    LruKPolicy policy(2);
    assert(policy.choose_victim(frames.get(), 3) == 0);
  }

  printf("test_replacement: OK\n");
  return 0;
}
