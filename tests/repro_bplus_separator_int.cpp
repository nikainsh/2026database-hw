/*
 * INT-key variant of the deterministic separator reproducer.
 *
 * This is intentionally not named tests/test_*.cpp: the normal suite remains
 * green until the B+Tree delete path is fixed.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "common/file_util.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/index/bplus_tree.h"

static const std::string kDir = "./_test_repro_int";
static const int         kKeyCount = 4000;

static std::string make_key(int value)
{
  std::string key(sizeof(int32_t), '\0');
  memcpy(&key[0], &value, sizeof(value));
  return key;
}

static RID make_rid(int value) { return RID(value * 3 + 1, value % 7); }

int main()
{
  std::error_code ec;
  std::filesystem::remove_all(kDir, ec);
  assert(OB_SUCC(path_mkdir_recursive(kDir)));

  DiskBufferPoolManager bpm(64, 256);
  assert(OB_SUCC(bpm.create_file(kDir + "/bug.idx")));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(kDir + "/bug.idx", ReadWriteMode::READ_WRITE, &pool)));

  BplusTree tree;
  assert(OB_SUCC(tree.open(pool, AttrType::INT, sizeof(int32_t), true, true)));
  for (int i = 0; i < kKeyCount; ++i) {
    assert(OB_SUCC(tree.insert_entry(make_key(i).c_str(), make_rid(i))));
  }

  PageHandle header_handle;
  assert(OB_SUCC(pool->get_page_read(INDEX_HEADER_PAGE, header_handle)));
  const IndexFileHeader &head = *reinterpret_cast<const IndexFileHeader *>(header_handle.const_data());
  PageNum                page = head.first_leaf_page;
  header_handle.reset();

  PageNum target_page = INVALID_PAGE_NUM;
  int32_t target_key  = -1;
  int     leaf_index  = 0;
  while (page != INVALID_PAGE_NUM) {
    PageHandle handle;
    assert(OB_SUCC(pool->get_page_read(page, handle)));
    const IndexNodeHeader &node = *reinterpret_cast<const IndexNodeHeader *>(handle.const_data());
    int32_t                first = 0;
    memcpy(&first, handle.const_data() + sizeof(IndexNodeHeader), sizeof(first));

    /// The second leaf is a non-leftmost child and still above min_keys after
    /// deleting its first key, so the underflow path is never entered.
    if (leaf_index == 1 && node.num_key > head.min_keys) {
      target_page = page;
      target_key  = first;
      break;
    }
    page = node.next_leaf;
    ++leaf_index;
  }

  assert(target_page != INVALID_PAGE_NUM && target_key >= 0);
  assert(OB_SUCC(tree.delete_entry(make_key(target_key).c_str(), make_rid(target_key))));

  const RC rc = tree.validate();
  printf("int reproducer: page=%d delete_key=%d validate_rc=%d\n", target_page, target_key,
         static_cast<int>(rc));

  assert(OB_SUCC(tree.close()));
  assert(OB_SUCC(bpm.close()));
  std::filesystem::remove_all(kDir, ec);

  if (rc != RC::SUCCESS) {
    printf("BUG REPRODUCED: INT separator mismatch after deleting leaf-1 min key %d\n", target_key);
    return 1;
  }
  printf("INT scenario passed (tree may already be fixed)\n");
  return 0;
}
