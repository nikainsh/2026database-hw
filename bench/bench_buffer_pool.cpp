/* database26 - buffer manager microbenchmark */
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"

static const std::string kDir  = "./_bench";
static const std::string kFile = "./_bench/pages.dat";

static void cleanup()
{
  path_remove_file(kFile);
  path_remove_file(kFile + ".wal");
}

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 128; ++i) {
    const int value = seed + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static void write_page(DiskBufferPool &pool, PageNum page, int seed)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_write(page, handle))) {
    std::fprintf(stderr, "write get_page failed: page=%d\n", page);
    return;
  }
  fill_page(handle.value(), seed);
  handle.mark_dirty();
}

static void read_page(DiskBufferPool &pool, PageNum page)
{
  PageHandle handle;
  if (OB_FAIL(pool.get_page_read(page, handle))) {
    std::fprintf(stderr, "read get_page failed: page=%d\n", page);
  }
}

using Clock = std::chrono::steady_clock;

static double elapsed_ms(Clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() / 1000.0;
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  cleanup();

  constexpr int kPages = 512;

  {
    DiskBufferPoolManager bpm(64, 2048);
    if (OB_FAIL(bpm.create_file(kFile))) {
      std::fprintf(stderr, "create_file failed\n");
      return 1;
    }
    DiskBufferPool *pool = nullptr;
    if (OB_FAIL(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool))) {
      std::fprintf(stderr, "open_file failed\n");
      return 1;
    }
    for (int i = 0; i < kPages; ++i) {
      PageNum page = INVALID_PAGE_NUM;
      if (OB_FAIL(pool->allocate_page(&page))) {
        std::fprintf(stderr, "allocate_page failed\n");
        return 1;
      }
      write_page(*pool, page, i);
    }
    if (OB_FAIL(bpm.close())) {
      std::fprintf(stderr, "close failed\n");
      return 1;
    }
  }

  {
    DiskBufferPoolManager bpm(64, 2048);
    DiskBufferPool       *pool = nullptr;
    if (OB_FAIL(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool))) {
      std::fprintf(stderr, "reopen failed\n");
      return 1;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, kPages);

    auto start = Clock::now();
    constexpr int kRandomOps = 20000;
    for (int i = 0; i < kRandomOps; ++i) {
      read_page(*pool, dist(rng));
    }
    const double random_ms = elapsed_ms(start);

    start = Clock::now();
    constexpr int kSequentialPasses = 10;
    for (int pass = 0; pass < kSequentialPasses; ++pass) {
      for (int page = 1; page <= kPages; ++page) {
        read_page(*pool, page);
      }
    }
    const double sequential_ms = elapsed_ms(start);

    start = Clock::now();
    constexpr int kMixedOps = 5000;
    for (int i = 0; i < kMixedOps; ++i) {
      const PageNum page = dist(rng);
      if ((i % 5) == 0) {
        write_page(*pool, page, i);
      } else {
        read_page(*pool, page);
      }
    }
    const double mixed_ms = elapsed_ms(start);

    const BufferPoolStatsSnapshot stats = pool->stats();
    std::printf("pages=%d frame_limit=%d global_limit=%d\n", kPages, pool->frame_limit(), bpm.total_frame_limit());
    std::printf("random_read:   ops=%d time=%.1f ms throughput=%.0f ops/s\n",
                kRandomOps, random_ms, kRandomOps * 1000.0 / (random_ms > 0 ? random_ms : 1));
    std::printf("sequential:    ops=%d time=%.1f ms throughput=%.0f ops/s\n",
                kSequentialPasses * kPages, sequential_ms,
                kSequentialPasses * kPages * 1000.0 / (sequential_ms > 0 ? sequential_ms : 1));
    std::printf("mixed_80_20:   ops=%d time=%.1f ms throughput=%.0f ops/s\n",
                kMixedOps, mixed_ms, kMixedOps * 1000.0 / (mixed_ms > 0 ? mixed_ms : 1));
    std::printf("hits=%llu misses=%llu evictions=%llu dirty_evictions=%llu reads=%llu writes=%llu\n",
                (unsigned long long)stats.hits, (unsigned long long)stats.misses,
                (unsigned long long)stats.evictions, (unsigned long long)stats.dirty_evictions,
                (unsigned long long)stats.reads, (unsigned long long)stats.writes);

    if (OB_FAIL(bpm.close())) {
      std::fprintf(stderr, "final close failed\n");
      return 1;
    }
  }

  cleanup();
  return 0;
}
