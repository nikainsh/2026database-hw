/* database26 buffer manager - WAL unit test */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "common/file_util.h"
#include "storage/buffer/page.h"
#include "storage/buffer/page_wal.h"

static const std::string kDir = "./_test_wal";
static const std::string kWal = "./_test_wal/pages.wal";

static void fill_page(char *page, int seed)
{
  memset(page, 0, BP_PAGE_SIZE);
  for (int i = 0; i < 64; ++i) {
    const int value = seed * 1000 + i;
    memcpy(page + i * sizeof(int), &value, sizeof(value));
  }
}

static void check_page(const char *page, int seed)
{
  for (int i = 0; i < 64; ++i) {
    int value = 0;
    memcpy(&value, page + i * sizeof(int), sizeof(value));
    assert(value == seed * 1000 + i);
  }
}

static void remove_files()
{
  if (file_exist(kWal)) {
    path_remove_file(kWal);
  }
}

static void test_append_replay_checkpoint()
{
  remove_files();
  assert(OB_SUCC(path_mkdir_recursive(kDir)));

  char page1[BP_PAGE_SIZE];
  char page2[BP_PAGE_SIZE];
  fill_page(page1, 1);
  fill_page(page2, 2);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_and_sync(1, page1)));
    assert(OB_SUCC(wal.append_and_sync(2, page2)));
    assert(wal.last_lsn() == 2);
    assert(OB_SUCC(wal.close()));
  }

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(false)));
    std::map<PageNum, std::string> restored;
    assert(OB_SUCC(wal.replay([&](PageNum page_num, const char *data) {
      restored[page_num] = std::string(data, BP_PAGE_SIZE);
      return RC::SUCCESS;
    })));
    assert(restored.size() == 2);
    check_page(restored[1].data(), 1);
    check_page(restored[2].data(), 2);
    assert(OB_SUCC(wal.checkpoint()));
    assert(file_size(kWal) == 16);
    assert(!file_exist(kWal + ".tmp"));
    assert(OB_SUCC(wal.close()));
  }
}

static void test_two_page_atomic_record()
{
  remove_files();
  assert(OB_SUCC(path_mkdir_recursive(kDir)));

  char page_a[BP_PAGE_SIZE];
  char page_b[BP_PAGE_SIZE];
  fill_page(page_a, 31);
  fill_page(page_b, 32);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_two_pages_and_sync(5, page_a, 6, page_b)));
    assert(wal.last_lsn() == 1);
    assert(OB_SUCC(wal.close()));
  }

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(false)));
    std::map<PageNum, std::string> restored;
    assert(OB_SUCC(wal.replay([&](PageNum page_num, const char *data) {
      restored[page_num] = std::string(data, BP_PAGE_SIZE);
      return RC::SUCCESS;
    })));
    assert(restored.size() == 2);
    check_page(restored[5].data(), 31);
    check_page(restored[6].data(), 32);
    assert(OB_SUCC(wal.close()));
  }

  /// Corrupting the second page invalidates the whole record, so neither page
  /// may be replayed.
  const long second_page_offset = 16 + 32 + BP_PAGE_SIZE + 10;
  FILE      *fp                 = fopen(kWal.c_str(), "r+b");
  assert(fp != nullptr);
  assert(fseek(fp, second_page_offset, SEEK_SET) == 0);
  int byte = 0;
  assert(fread(&byte, 1, 1, fp) == 1);
  byte ^= 0xFF;
  assert(fseek(fp, second_page_offset, SEEK_SET) == 0);
  assert(fwrite(&byte, 1, 1, fp) == 1);
  fclose(fp);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(false)));
    int count = 0;
    assert(OB_SUCC(wal.replay([&](PageNum, const char *) {
      ++count;
      return RC::SUCCESS;
    })));
    assert(count == 0);
    assert(OB_SUCC(wal.close()));
  }
}

static void test_torn_tail()
{
  remove_files();
  char page1[BP_PAGE_SIZE];
  fill_page(page1, 7);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_and_sync(1, page1)));
    assert(OB_SUCC(wal.close()));
  }

  FILE *fp = fopen(kWal.c_str(), "ab");
  assert(fp != nullptr);
  const char garbage[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  assert(fwrite(garbage, 1, sizeof(garbage), fp) == sizeof(garbage));
  fclose(fp);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(false)));
    int count = 0;
    assert(OB_SUCC(wal.replay([&](PageNum, const char *data) {
      check_page(data, 7);
      ++count;
      return RC::SUCCESS;
    })));
    assert(count == 1);
    assert(OB_SUCC(wal.close()));
  }
}

static void test_crc_corruption_stops_replay()
{
  remove_files();
  char page1[BP_PAGE_SIZE];
  char page2[BP_PAGE_SIZE];
  fill_page(page1, 11);
  fill_page(page2, 22);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(true)));
    assert(OB_SUCC(wal.append_and_sync(1, page1)));
    assert(OB_SUCC(wal.append_and_sync(2, page2)));
    assert(OB_SUCC(wal.close()));
  }

  /// 16-byte WAL header + 32-byte record header + 8KB page.
  const long second_data_offset = 16 + 32 + BP_PAGE_SIZE + 32 + 10;
  FILE      *fp                 = fopen(kWal.c_str(), "r+b");
  assert(fp != nullptr);
  assert(fseek(fp, second_data_offset, SEEK_SET) == 0);
  int byte = 0;
  assert(fread(&byte, 1, 1, fp) == 1);
  byte ^= 0xFF;
  assert(fseek(fp, second_data_offset, SEEK_SET) == 0);
  assert(fwrite(&byte, 1, 1, fp) == 1);
  fclose(fp);

  {
    PageWAL wal(kWal);
    assert(OB_SUCC(wal.open(false)));
    int count = 0;
    assert(OB_SUCC(wal.replay([&](PageNum page_num, const char *data) {
      assert(page_num == 1);
      check_page(data, 11);
      ++count;
      return RC::SUCCESS;
    })));
    assert(count == 1);
    assert(OB_SUCC(wal.close()));
  }
}

int main()
{
  test_append_replay_checkpoint();
  test_two_page_atomic_record();
  test_torn_tail();
  test_crc_corruption_stops_replay();
  remove_files();
  printf("test_wal: OK\n");
  return 0;
}
