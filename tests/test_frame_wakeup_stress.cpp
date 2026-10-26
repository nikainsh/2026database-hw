/* database26 buffer manager - pin wakeups must not be lost under prefetch */
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/frame_wakeup.dat";
static constexpr int      kPages = 8;
static constexpr int      kIterations = 20000;

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  DiskBufferPoolManager bpm(1, 1);
  assert(OB_SUCC(bpm.create_file(kFile)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

  for (int i = 0; i < kPages; ++i) {
    PageNum page = INVALID_PAGE_NUM;
    assert(OB_SUCC(pool->allocate_page(&page)));
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_write(page, handle)));
    memset(handle.value(), i + 1, BP_PAGE_SIZE);
    assert(OB_SUCC(handle.mark_dirty()));
  }

  std::atomic<bool> done{false};
  std::atomic<int>  failures{0};

  std::thread worker([&]() {
    for (int round = 0; round < kIterations; ++round) {
      const PageNum page = 1 + (round % kPages);
      if (OB_FAIL(pool->prefetch_page(page))) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }

      PageHandle handle;
      RC rc = pool->get_page_read(page, handle);
      if (OB_FAIL(rc)) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      handle.reset();

      /// dispose_page() must wait out temporary background pins instead of
      /// returning PAGE_PINNED or hanging.
      if ((round % 53) == 0) {
        PageNum reuse = INVALID_PAGE_NUM;
        RC rc2 = pool->dispose_page(page);
        if (OB_FAIL(rc2)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        } else if (OB_FAIL(pool->allocate_page(&reuse))) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    done.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!done.load(std::memory_order_acquire)) {
    fprintf(stderr, "test_frame_wakeup_stress: timed out (lost wakeup?)\n");
    std::_Exit(1);
  }
  worker.join();
  assert(failures.load() == 0);

  assert(OB_SUCC(bpm.close()));
  cleanup();
  printf("test_frame_wakeup_stress: OK\n");
  return 0;
}
