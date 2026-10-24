/* database26 buffer manager - multi-threaded smoke test */
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_test_buf";
static const std::string kFile = "./_test_buf/bp_threads.dat";

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static bool write_page(DiskBufferPool &pool, PageNum page_num, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_write(page_num, handle))) {
    return false;
  }
  fill_page(handle.value(), seed);
  if (OB_FAIL(handle.mark_dirty())) {
    return false;
  }
  handle.reset();
  return true;
}

static bool check_page(DiskBufferPool &pool, PageNum page_num, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_read(page_num, handle))) {
    return false;
  }
  const int *words = reinterpret_cast<const int *>(handle.const_data());
  for (int i = 0; i < 64; ++i) {
    if (words[i] != seed * 1000 + i) {
      return false;
    }
  }
  handle.reset();
  return true;
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  if (file_exist(kFile)) {
    assert(OB_SUCC(path_remove_file(kFile)));
  }

  constexpr int kPages    = 8;
  constexpr int kThreads  = 4;
  constexpr int kIters    = 25;
  constexpr int kPerThread = kPages / kThreads;

  {
    DiskBufferPoolManager bpm(64, 256);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

    std::vector<PageNum> pages(kPages);
    for (int i = 0; i < kPages; ++i) {
      assert(OB_SUCC(pool->allocate_page(&pages[i])));
      assert(write_page(*pool, pages[i], 1000 + i));
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t]() {
        const int begin = t * kPerThread;
        const int end   = begin + kPerThread;
        for (int iter = 0; iter < kIters; ++iter) {
          for (int i = begin; i < end; ++i) {
            const int seed = t * 10000 + iter * 10 + i;
            if (!write_page(*pool, pages[i], seed) || !check_page(*pool, pages[i], seed)) {
              failures.fetch_add(1);
              return;
            }
          }
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    assert(failures.load() == 0);
    assert(OB_SUCC(bpm.close()));
  }

  {
    DiskBufferPoolManager bpm(64, 256);
    DiskBufferPool       *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));
    assert(pool->page_count() == kPages + 1);
    for (int t = 0; t < kThreads; ++t) {
      const int begin = t * kPerThread;
      const int end   = begin + kPerThread;
      for (int i = begin; i < end; ++i) {
        const int seed = t * 10000 + (kIters - 1) * 10 + i;
        assert(check_page(*pool, static_cast<PageNum>(i + 1), seed));
      }
    }
    assert(OB_SUCC(bpm.close()));
  }

  printf("test_buffer_pool_threads: OK\n");
  return 0;
}
