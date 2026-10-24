/* database26 - concurrent B+Tree smoke test (tree-level latch + page locks) */
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/index/bplus_tree.h"

static const std::string kDir  = "./_test_index";
static const std::string kFile = "./_test_index/concurrent.index";

static void remove_files()
{
  if (file_exist(kFile)) {
    path_remove_file(kFile);
  }
  if (file_exist(kFile + ".wal")) {
    path_remove_file(kFile + ".wal");
  }
}

int main()
{
  assert(OB_SUCC(path_mkdir_recursive(kDir)));
  remove_files();

  constexpr int kThreads    = 4;
  constexpr int kPerThread  = 100;
  constexpr int kTotal      = kThreads * kPerThread;

  {
    DiskBufferPoolManager bpm(64, 256);
    assert(OB_SUCC(bpm.create_file(kFile)));
    DiskBufferPool *pool = nullptr;
    assert(OB_SUCC(bpm.open_file(kFile, ReadWriteMode::READ_WRITE, &pool)));

    BplusTree tree;
    assert(OB_SUCC(tree.open(pool, AttrType::INT, static_cast<int>(sizeof(int32_t)), false, true)));

    std::atomic<int> failures{0};

    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
      writers.emplace_back([&, t]() {
        for (int i = 0; i < kPerThread; ++i) {
          int32_t key = t * kPerThread + i;
          RID     rid(1, key);
          if (OB_FAIL(tree.insert_entry(reinterpret_cast<const char *>(&key), rid))) {
            failures.fetch_add(1);
            return;
          }
        }
      });
    }
    for (auto &thread : writers) {
      thread.join();
    }
    assert(failures.load() == 0);
    assert(tree.entry_count() == kTotal);

    std::vector<std::thread> readers;
    for (int t = 0; t < kThreads; ++t) {
      readers.emplace_back([&, t]() {
        for (int i = 0; i < kPerThread; ++i) {
          int32_t key = t * kPerThread + i;
          std::vector<RID> rids;
          if (OB_FAIL(tree.get_entry(reinterpret_cast<const char *>(&key), rids)) || rids.size() != 1 ||
              rids[0].slot_num != key) {
            failures.fetch_add(1);
            return;
          }
        }
      });
    }
    for (auto &thread : readers) {
      thread.join();
    }
    assert(failures.load() == 0);

    std::vector<std::thread> deleters;
    for (int t = 0; t < kThreads; ++t) {
      deleters.emplace_back([&, t]() {
        for (int i = 0; i < kPerThread; ++i) {
          int32_t key = t * kPerThread + i;
          if ((key & 1) != 0) {
            continue;
          }
          RID rid(1, key);
          if (OB_FAIL(tree.delete_entry(reinterpret_cast<const char *>(&key), rid))) {
            failures.fetch_add(1);
            return;
          }
        }
      });
    }
    for (auto &thread : deleters) {
      thread.join();
    }
    assert(failures.load() == 0);
    assert(tree.entry_count() == kTotal / 2);

    for (int key = 0; key < kTotal; ++key) {
      std::vector<RID> rids;
      assert(OB_SUCC(tree.get_entry(reinterpret_cast<const char *>(&key), rids)));
      if ((key & 1) == 0) {
        assert(rids.empty());
      } else {
        assert(rids.size() == 1 && rids[0].slot_num == key);
      }
    }

    assert(OB_SUCC(tree.close()));
    assert(OB_SUCC(bpm.close()));
  }

  remove_files();
  printf("test_concurrent_bplus_tree: OK\n");
  return 0;
}
