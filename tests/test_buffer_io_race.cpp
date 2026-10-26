/* database26 buffer manager - data-file reads must not race with prefetch */
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/io_race.dat";
static constexpr int      kPages = 256;

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static bool check_page(PageHandle &handle, int seed)
{
  const int *words = reinterpret_cast<const int *>(handle.const_data());
  for (int i = 0; i < 64; ++i) {
    if (words[i] != seed * 1000 + i) {
      return false;
    }
  }
  return true;
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  {
    DiskBufferPoolManager bpm(16, 64);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    for (int i = 0; i < kPages; ++i) {
      PageNum page = INVALID_PAGE_NUM;
      assert(OB_SUCC(pool->allocate_page(&page)));
      PageHandle handle;
      assert(OB_SUCC(pool->get_page_write(page, handle)));
      fill_page(handle.value(), i + 1);
      assert(OB_SUCC(handle.mark_dirty()));
    }
    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(8, 8);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_ONLY, &pool)));

    std::atomic<bool> stop{false};
    std::atomic<int>  failures{0};

    std::vector<std::thread> prefetchers;
    for (int t = 0; t < 2; ++t) {
      prefetchers.emplace_back([&, t]() {
        int round = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          const PageNum page = 1 + ((round++ * 37 + t * 11) % kPages);
          if (OB_FAIL(pool->prefetch_page(page))) {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
          std::this_thread::yield();
        }
      });
    }

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&, t]() {
        std::minstd_rand rng(static_cast<unsigned>(t + 1));
        for (int i = 0; i < 20000; ++i) {
          const PageNum page = 1 + static_cast<PageNum>(rng() % kPages);
          PageHandle handle;
          RC rc = pool->get_page_read(page, handle);
          if (OB_FAIL(rc) || !check_page(handle, static_cast<int>(page))) {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true, std::memory_order_relaxed);
    for (auto &thread : prefetchers) {
      thread.join();
    }
    for (auto &thread : readers) {
      thread.join();
    }
    assert(failures.load() == 0);
    assert(OB_SUCC(bpm.close()));
  }

  cleanup();
  printf("test_buffer_io_race: OK\n");
  return 0;
}
