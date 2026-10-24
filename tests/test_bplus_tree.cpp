/* minidb - B+Tree 自测：用「同比较规则的参照模型」校验插入/删除/点查/范围扫描 */

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/index/bplus_tree.h"

// ---------------------------------------------------------------------------
// 参照模型：排序规则必须和 B+Tree 的 compare_keys 一致
// ---------------------------------------------------------------------------

using KeyLess = bool (*)(const std::string &, const std::string &);

static int32_t key_int(const std::string &key)
{
  int32_t v = 0;
  memcpy(&v, key.data(), sizeof(v));
  return v;
}

static int64_t key_bigint(const std::string &key)
{
  int64_t v = 0;
  memcpy(&v, key.data(), sizeof(v));
  return v;
}

static bool less_int(const std::string &a, const std::string &b) { return key_int(a) < key_int(b); }
static bool less_bigint(const std::string &a, const std::string &b) { return key_bigint(a) < key_bigint(b); }
/// CHARS 的键定长且用 '\0' 补齐，字节序比较与 memcmp 一致
static bool less_chars(const std::string &a, const std::string &b) { return a < b; }

static bool equal_key(KeyLess less, const std::string &a, const std::string &b)
{
  return !less(a, b) && !less(b, a);
}

struct Model
{
  KeyLess                                                   less  = less_int;
  std::vector<std::pair<std::string, RID>>                  items;

  void insert(const std::string &key, const RID &rid) { items.emplace_back(key, rid); }

  bool erase(const std::string &key, const RID &rid)
  {
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (equal_key(less, it->first, key) && it->second == rid) {
        items.erase(it);
        return true;
      }
    }
    return false;
  }

  std::vector<std::pair<std::string, RID>> sorted() const
  {
    auto copy = items;
    std::stable_sort(copy.begin(), copy.end(),
                     [this](const auto &a, const auto &b) { return less(a.first, b.first); });
    return copy;
  }

  std::vector<RID> lookup(const std::string &key) const
  {
    std::vector<RID> out;
    for (const auto &item : items) {
      if (equal_key(less, item.first, key)) {
        out.push_back(item.second);
      }
    }
    return out;
  }

  std::vector<std::pair<std::string, RID>> range(
      const std::string *low, bool low_incl, const std::string *high, bool high_incl) const
  {
    std::vector<std::pair<std::string, RID>> out;
    for (const auto &item : sorted()) {
      const bool ok_low  = low == nullptr || (low_incl ? !less(item.first, *low) : less(*low, item.first));
      const bool ok_high = high == nullptr || (high_incl ? !less(*high, item.first) : less(item.first, *high));
      if (ok_low && ok_high) {
        out.push_back(item);
      }
    }
    return out;
  }

  size_t count() const { return items.size(); }
};

// ---------------------------------------------------------------------------
// 树侧的辅助
// ---------------------------------------------------------------------------

static std::string int_key(int v)
{
  std::string key(sizeof(int32_t), '\0');
  int32_t     x = v;
  memcpy(&key[0], &x, sizeof(x));
  return key;
}

static std::string bigint_key(int64_t v)
{
  std::string key(sizeof(int64_t), '\0');
  memcpy(&key[0], &v, sizeof(v));
  return key;
}

static RID make_rid(int i) { return RID(i * 3 + 1, i % 7); }

static std::vector<std::pair<std::string, RID>> scan_all(BplusTree &tree)
{
  BplusTreeScanner scanner;
  assert(OB_SUCC(tree.make_scanner(scanner, nullptr, true, nullptr, true)));

  std::vector<std::pair<std::string, RID>> out;
  std::string                              key;
  RID                                      rid;
  while (true) {
    RC rc = scanner.next(key, rid);
    if (rc == RC::RECORD_EOF) break;
    assert(OB_SUCC(rc));
    out.emplace_back(key, rid);
  }
  return out;
}

static std::vector<std::pair<std::string, RID>> scan_range(
    BplusTree &tree, const std::string *low, bool low_incl, const std::string *high, bool high_incl)
{
  BplusTreeScanner scanner;
  assert(OB_SUCC(tree.make_scanner(
      scanner, low ? low->c_str() : nullptr, low_incl, high ? high->c_str() : nullptr, high_incl)));

  std::vector<std::pair<std::string, RID>> out;
  std::string                              key;
  RID                                      rid;
  while (true) {
    RC rc = scanner.next(key, rid);
    if (rc == RC::RECORD_EOF) break;
    assert(OB_SUCC(rc));
    out.emplace_back(key, rid);
  }
  return out;
}

/// 相同键上的条目顺序属于实现细节，因此按键分组比较 RID 集合
static void check_same_entries(const std::vector<std::pair<std::string, RID>> &a,
                               const std::vector<std::pair<std::string, RID>> &b, const char *where)
{
  printf("check[%s] tree=%d model=%d\n", where, (int)a.size(), (int)b.size());
  auto group = [](const std::vector<std::pair<std::string, RID>> &in) {
    std::vector<std::pair<std::string, std::vector<RID>>> out;
    for (const auto &item : in) {
      if (out.empty() || out.back().first != item.first) {
        out.emplace_back(item.first, std::vector<RID>());
      }
      out.back().second.push_back(item.second);
    }
    for (auto &entry : out) {
      std::sort(entry.second.begin(), entry.second.end(),
                [](const RID &x, const RID &y) { return x.compare(y) < 0; });
    }
    return out;
  };

  const auto ga = group(a);
  const auto gb = group(b);
  if (ga.size() != gb.size()) {
    printf("group count mismatch: %d vs %d\n", (int)ga.size(), (int)gb.size());
    assert(false);
  }
  for (size_t i = 0; i < ga.size(); ++i) {
    if (ga[i].first != gb[i].first || ga[i].second != gb[i].second) {
      printf("MISMATCH at group %d key=%d\n", (int)i, key_int(ga[i].first));
      printf("  tree:");
      for (const RID &r : ga[i].second) printf(" %s", r.to_string().c_str());
      printf("\n  model:");
      for (const RID &r : gb[i].second) printf(" %s", r.to_string().c_str());
      printf("\n");

      /// 找出缺失的 rid 在树里挂到了哪个键上
      for (size_t j = 0; j < gb[i].second.size(); ++j) {
        const RID &want = gb[i].second[j];
        if (std::find(ga[i].second.begin(), ga[i].second.end(), want) == ga[i].second.end()) {
          printf("  missing rid %s found under tree key: ", want.to_string().c_str());
          for (size_t k = 0; k < ga.size(); ++k) {
            if (std::find(ga[k].second.begin(), ga[k].second.end(), want) != ga[k].second.end()) {
              printf("%d ", key_int(ga[k].first));
            }
          }
          printf("\n");
        }
      }

      /// 反向：树里有、模型里没有的条目挂在哪个键上
      for (size_t j = 0; j < ga[i].second.size(); ++j) {
        const RID &got = ga[i].second[j];
        if (std::find(gb[i].second.begin(), gb[i].second.end(), got) == gb[i].second.end()) {
          printf("  extra rid %s expected under model key: ", got.to_string().c_str());
          for (size_t k = 0; k < gb.size(); ++k) {
            if (std::find(gb[k].second.begin(), gb[k].second.end(), got) != gb[k].second.end()) {
              printf("%d ", key_int(gb[k].first));
            }
          }
          printf("\n");
        }
      }
      assert(false);
    }
  }
}

/// 结构与内容都要和参照模型一致
static void check_model(BplusTree &tree, Model &model)
{
  std::vector<std::string> keys;
  assert(OB_SUCC(tree.validate(&keys)));

  auto all = scan_all(tree);
  assert(all.size() == model.count());
  assert(static_cast<int64_t>(all.size()) == tree.entry_count());
  check_same_entries(all, model.sorted(), "check_model");

  /// 点查
  auto sorted_keys = model.sorted();
  for (size_t i = 0; i < sorted_keys.size(); ++i) {
    if (i > 0 && !equal_key(model.less, sorted_keys[i].first, sorted_keys[i - 1].first)) {
      continue;
    }
    std::vector<RID> rids;
    assert(OB_SUCC(tree.get_entry(sorted_keys[i].first.c_str(), rids)));
    auto want = model.lookup(sorted_keys[i].first);
    assert(rids.size() == want.size());
    for (const RID &rid : want) {
      assert(std::find(rids.begin(), rids.end(), rid) != rids.end());
    }
  }

  /// 不存在的键查不到
  for (int i = 0; i < 20; ++i) {
    std::string key = int_key(1000000 + i);
    assert(model.lookup(key).empty());
    bool exists = true;
    assert(OB_SUCC(tree.entry_exists(key.c_str(), exists)));
    assert(!exists);
  }
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

static void test_int_keys(bool unique, const char *path)
{
  path_mkdir_recursive("./_test_index");
  if (file_exist(path)) {
    assert(OB_SUCC(path_remove_file(path)));
  }

  DiskBufferPoolManager bpm(24);
  assert(OB_SUCC(bpm.create_file(path)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(path, ReadWriteMode::READ_WRITE, &pool)));

  BplusTree tree;
  assert(OB_SUCC(tree.open(pool, AttrType::INT, sizeof(int32_t), unique, true)));

  std::mt19937 rng(20261019);
  Model        model;
  model.less = less_int;

  const int kInserts = unique ? 3000 : 4000;
  for (int i = 0; i < kInserts; ++i) {
    /// 非唯一索引故意收窄值域，制造大量重复键
    const int v   = unique ? i : static_cast<int>(rng() % 900);
    std::string key = int_key(v);
    const RID   rid = make_rid(static_cast<int>(model.count()));

    assert(OB_SUCC(tree.insert_entry(key.c_str(), rid)));
    model.insert(key, rid);

    if (unique) {
      assert(tree.insert_entry(key.c_str(), RID(9999, 9999)) == RC::RECORD_DUPLICATE_KEY);
    }
  }
  check_model(tree, model);

  /// 随机删掉一半，期间反复验证树仍然是平衡且正确的
  auto entries = model.items;
  std::shuffle(entries.begin(), entries.end(), rng);
  const size_t remove_count = entries.size() / 2;
  for (size_t i = 0; i < remove_count; ++i) {
    assert(OB_SUCC(tree.delete_entry(entries[i].first.c_str(), entries[i].second)));
    assert(model.erase(entries[i].first, entries[i].second));
    if ((i & 0x3F) == 0x3F) {
      check_model(tree, model);
    }
  }
  check_model(tree, model);

  /// 删不存在的键必须报错
  {
    std::string key = int_key(-123456);
    assert(tree.delete_entry(key.c_str(), RID(1, 1)) == RC::RECORD_NOT_EXIST);
  }

  /// 范围扫描：四个方向的开闭区间都要对
  for (int round = 0; round < 40; ++round) {
    const int   low  = static_cast<int>(rng() % 1000) - 100;
    const int   high = low + static_cast<int>(rng() % 220);
    std::string low_key = int_key(low), high_key = int_key(high);
    const bool  low_incl  = (round % 2) == 0;
    const bool  high_incl = (round % 3) != 0;

    auto got    = scan_range(tree, &low_key, low_incl, &high_key, high_incl);
    auto expect = model.range(&low_key, low_incl, &high_key, high_incl);
    check_same_entries(got, expect, "int_range");

    /// 只给下界
    auto tail = scan_range(tree, &low_key, low_incl, nullptr, true);
    assert(tail.size() == model.range(&low_key, low_incl, nullptr, true).size());
  }

  /// 清空后树必须是空的
  entries = model.items;
  for (const auto &item : entries) {
    assert(OB_SUCC(tree.delete_entry(item.first.c_str(), item.second)));
    model.erase(item.first, item.second);
  }
  assert(tree.entry_count() == 0);
  assert(scan_all(tree).empty());
  check_model(tree, model);

  /// 重新灌数据，关掉再打开，验证持久化正确
  for (int i = 0; i < 500; ++i) {
    std::string key = int_key(i);
    RID         rid = make_rid(i);
    assert(OB_SUCC(tree.insert_entry(key.c_str(), rid)));
    model.insert(key, rid);
  }
  assert(OB_SUCC(bpm.close()));

  DiskBufferPoolManager bpm2(24);
  DiskBufferPool       *pool2 = nullptr;
  assert(OB_SUCC(bpm2.open_file(path, ReadWriteMode::READ_WRITE, &pool2)));
  BplusTree reopened;
  assert(OB_SUCC(reopened.open(pool2, AttrType::INT, sizeof(int32_t), unique, false)));
  check_model(reopened, model);
  assert(OB_SUCC(bpm2.close()));
}

static void test_char_keys()
{
  const char *path = "./_test_index/name.idx";
  if (file_exist(path)) {
    assert(OB_SUCC(path_remove_file(path)));
  }

  DiskBufferPoolManager bpm(16);
  assert(OB_SUCC(bpm.create_file(path)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(path, ReadWriteMode::READ_WRITE, &pool)));

  const int kKeyLen = 8;
  BplusTree tree;
  assert(OB_SUCC(tree.open(pool, AttrType::CHARS, kKeyLen, false, true)));

  auto make_key = [](int i) {
    char buf[16];
    snprintf(buf, sizeof(buf), "name%05d", i);
    std::string key(kKeyLen, '\0');
    memcpy(&key[0], buf, std::min<size_t>(strlen(buf), kKeyLen));
    return key;
  };

  std::mt19937 rng(7);
  Model        model;
  model.less = less_chars;
  for (int i = 0; i < 1500; ++i) {
    std::string key = make_key(static_cast<int>(rng() % 1200));
    RID         rid = make_rid(i);
    assert(OB_SUCC(tree.insert_entry(key.c_str(), rid)));
    model.insert(key, rid);
  }
  check_model(tree, model);

  /// 前缀范围 [name00100, name00200)
  std::string low  = make_key(100);
  std::string high = make_key(200);
  auto        got  = scan_range(tree, &low, true, &high, false);
  auto        expect = model.range(&low, true, &high, false);
  check_same_entries(got, expect, "char_range");

  if (!got.empty()) {
    /// 定长键补齐的 '\0' 在显示时必须被去掉
    assert(tree.key_to_string(got[0].first.c_str()) == std::string(got[0].first.c_str()));
  }

  assert(OB_SUCC(bpm.close()));
}

static void test_bigint_keys()
{
  const char *path = "./_test_index/num.idx";
  if (file_exist(path)) {
    assert(OB_SUCC(path_remove_file(path)));
  }

  DiskBufferPoolManager bpm(16);
  assert(OB_SUCC(bpm.create_file(path)));
  DiskBufferPool *pool = nullptr;
  assert(OB_SUCC(bpm.open_file(path, ReadWriteMode::READ_WRITE, &pool)));

  BplusTree tree;
  assert(OB_SUCC(tree.open(pool, AttrType::BIGINT, sizeof(int64_t), true, true)));

  std::vector<int64_t> values;
  for (int i = 0; i < 600; ++i) {
    values.push_back(static_cast<int64_t>(i) * 10000000000LL - 3000000000000LL);
  }
  std::shuffle(values.begin(), values.end(), std::mt19937(3));

  Model model;
  model.less = less_bigint;
  for (size_t i = 0; i < values.size(); ++i) {
    std::string key = bigint_key(values[i]);
    RID         rid = make_rid(static_cast<int>(i));
    assert(OB_SUCC(tree.insert_entry(key.c_str(), rid)));
    model.insert(key, rid);
  }
  check_model(tree, model);

  /// 负数与正数必须按数值排序，而不是按字节序
  auto all = scan_all(tree);
  assert(all.size() == values.size());
  for (size_t i = 1; i < all.size(); ++i) {
    assert(key_bigint(all[i - 1].first) < key_bigint(all[i].first));
  }

  assert(OB_SUCC(bpm.close()));
}

int main()
{
  test_int_keys(true, "./_test_index/u.idx");
  test_int_keys(false, "./_test_index/n.idx");
  test_char_keys();
  test_bigint_keys();
  printf("test_bplus_tree: OK\n");
  return 0;
}
