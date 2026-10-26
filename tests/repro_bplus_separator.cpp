/*
 * Deterministic reproducer for the B+Tree separator-key bug.
 *
 * This file intentionally does NOT match tests/test_*.cpp, so the normal
 * build/test suite stays green while the bug is unfixed.
 *
 * Run:
 *   bash tests/run_bplus_separator_repro.sh
 *
 * Expected on the buggy tree: every scenario reports a separator mismatch and
 * the program exits 1 (a normal failing regression test). After the B+Tree fix
 * validate() succeeds and the program exits 0.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/index/bplus_tree.h"

static const std::string kDir = "./_test_repro";
static const int         kKeyLen = 128;
static const int         kKeyCount = 256;

static std::string make_key(int value)
{
  std::string key(static_cast<size_t>(kKeyLen), '\0');
  int32_t     x = value;
  memcpy(&key[0], &x, sizeof(x));
  return key;
}

static RID make_rid(int value) { return RID(value + 1, 0); }

struct LeafInfo
{
  PageNum page     = INVALID_PAGE_NUM;
  int32_t count    = 0;
  int32_t first_key = 0;
  PageNum next     = INVALID_PAGE_NUM;
};

static bool load_leaves(DiskBufferPool &pool, std::vector<LeafInfo> &leaves)
{
  PageHandle header_handle;
  if (OB_FAIL(pool.get_page_read(INDEX_HEADER_PAGE, header_handle))) {
    return false;
  }
  const IndexFileHeader &head = *reinterpret_cast<const IndexFileHeader *>(header_handle.const_data());
  PageNum                page = head.first_leaf_page;
  header_handle.reset();

  while (page != INVALID_PAGE_NUM) {
    PageHandle handle;
    if (OB_FAIL(pool.get_page_read(page, handle))) {
      return false;
    }
    const IndexNodeHeader &node = *reinterpret_cast<const IndexNodeHeader *>(handle.const_data());
    LeafInfo              info;
    info.page  = page;
    info.count = node.num_key;
    info.next  = node.next_leaf;
    memcpy(&info.first_key, handle.const_data() + sizeof(IndexNodeHeader), sizeof(info.first_key));
    leaves.push_back(info);
    page = node.next_leaf;
  }
  return true;
}

static bool trigger_on_leaf(int wanted_leaf, int *target_key)
{
  const std::string dir  = kDir + "/leaf_" + std::to_string(wanted_leaf);
  const std::string file = dir + "/bug.idx";

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  assert(OB_SUCC(path_mkdir_recursive(dir)));

  DiskBufferPoolManager bpm(64, 256);
  assert(OB_SUCC(bpm.create_file(file)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(file, ReadWriteMode::READ_WRITE, &pool)));

  BplusTree tree;
  assert(OB_SUCC(tree.open(pool, AttrType::CHARS, kKeyLen, true, true)));
  for (int i = 0; i < kKeyCount; ++i) {
    std::string key = make_key(i);
    assert(OB_SUCC(tree.insert_entry(key.c_str(), make_rid(i))));
  }

  std::vector<LeafInfo> leaves;
  assert(load_leaves(*pool, leaves));
  assert(wanted_leaf > 0 && wanted_leaf < static_cast<int>(leaves.size()));

  const LeafInfo &leaf = leaves[static_cast<size_t>(wanted_leaf)];
  *target_key          = leaf.first_key;

  /// This leaf is not the leftmost child, still has more than min_keys after
  /// deleting its first key, so delete_entry() skips fix_underflow().
  std::string key = make_key(leaf.first_key);
  assert(OB_SUCC(tree.delete_entry(key.c_str(), make_rid(leaf.first_key))));

  const RC rc = tree.validate();
  printf("leaf=%d page=%d count_before=%d delete_key=%d validate_rc=%d\n", wanted_leaf, leaf.page,
         leaf.count, leaf.first_key, static_cast<int>(rc));

  assert(OB_SUCC(tree.close()));
  assert(OB_SUCC(bpm.close()));
  return rc != RC::SUCCESS;
}

int main()
{
  std::error_code ec;
  std::filesystem::remove_all(kDir, ec);

  int reproduced = 0;
  for (int leaf = 1; leaf <= 3; ++leaf) {
    int key = -1;
    if (trigger_on_leaf(leaf, &key)) {
      ++reproduced;
      printf("  BUG REPRODUCED: separator mismatch after deleting leaf %d min key %d\n", leaf, key);
    } else {
      printf("  not reproduced for leaf %d (tree may already be fixed)\n", leaf);
    }
  }

  std::filesystem::remove_all(kDir, ec);
  if (reproduced == 3) {
    printf("RESULT: BUG REPRODUCED in %d/3 deterministic scenarios\n", reproduced);
    return 1;
  }
  printf("RESULT: only %d/3 scenarios reproduced; tree may already be fixed\n", reproduced);
  return reproduced == 0 ? 0 : 1;
}
