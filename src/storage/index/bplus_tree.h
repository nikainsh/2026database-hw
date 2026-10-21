/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

#include "common/rc.h"
#include "common/value.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/record/record.h"

static constexpr PageNum INDEX_HEADER_PAGE = 1;  /// 第 0 页属于缓冲池
static constexpr PageNum INDEX_FIRST_NODE_PAGE = 2;
static constexpr int32_t INDEX_FILE_MAGIC = 0x4D494458;  /// 'MIDX'
static constexpr int     INDEX_VALUE_LEN = static_cast<int>(sizeof(PageNum) + sizeof(SlotNum));

/**
 * @brief 索引文件第 1 页：整棵树的全局信息
 */
struct IndexFileHeader
{
  int32_t magic = INDEX_FILE_MAGIC;
  int32_t key_type = 0;           /// AttrType
  int32_t key_len = 0;
  int32_t unique_flag = 0;
  int32_t order = 0;              /// 一个节点最多能放多少个 key（偶数）
  int32_t min_keys = 0;           /// order / 2
  PageNum root_page = INVALID_PAGE_NUM;
  PageNum first_leaf_page = INVALID_PAGE_NUM;
  int32_t tree_height = 0;
  int64_t entry_count = 0;        /// 叶子里的索引条目数
};

/**
 * @brief 索引节点页的页头
 * @details 叶子节点：num_key 个 (key, value=RID) 按 key 有序，用 next_leaf/prev_leaf 串成链表；
 *          内部节点：num_key 个 key + num_key+1 个孩子指针，key[i] 是 child[i+1] 子树的最小键。
 */
struct IndexNodeHeader
{
  int32_t num_key;
  int32_t is_leaf;
  PageNum next_leaf;
  PageNum prev_leaf;
};

class BplusTree;

/**
 * @brief 有序扫描游标：定位到叶子后沿 next_leaf 链向右走
 */
class BplusTreeScanner
{
public:
  /// left/right 为 nullptr 表示该方向不设界
  RC init(BplusTree *tree, const char *left_key, bool left_inclusive, const char *right_key, bool right_inclusive);

  /// 取下一条索引项，结束返回 RC::RECORD_EOF
  RC next(std::string &key, RID &rid);
  RC restart();

private:
  RC move_to_next_leaf();

  BplusTree *tree_ = nullptr;
  std::string left_key_, right_key_;
  bool        left_inclusive_  = true;
  bool        right_inclusive_ = true;

  PageNum  current_page_ = INVALID_PAGE_NUM;
  int32_t  current_slot_ = 0;
  PageHandle handle_;
  bool       finished_ = false;
};

/**
 * @brief B+Tree 索引：磁盘上的多路平衡搜索树
 * @details 对应 miniob 的 BplusTreeHandler（不含 WAL 与并发控制）。
 *          - 所有节点都存放在缓冲管理的页里，只有被修改的页才写回磁盘；
 *          - order 在建索引时按页大小算出，保证叶子与内部节点都放得下；
 *          - 插入分裂、删除借位/合并，树高只由根的增长/收缩变化。
 */
class BplusTree
{
public:
  BplusTree() = default;
  ~BplusTree();

  /// create=true 会建出一棵只含一个空叶子的树
  RC open(DiskBufferPool *buffer_pool, AttrType key_type, int key_len, bool unique, bool create);
  RC close();

  RC insert_entry(const char *key, const RID &rid);
  RC delete_entry(const char *key, const RID &rid);
  /// 精确匹配，唯一索引最多 1 条
  RC get_entry(const char *key, std::vector<RID> &rids) const;
  RC entry_exists(const char *key, bool &exists) const;

  RC make_scanner(BplusTreeScanner &scanner,
                  const char *left_key, bool left_inclusive,
                  const char *right_key, bool right_inclusive) const;

  int64_t entry_count() const { return header_.entry_count; }
  int     key_len() const { return header_.key_len; }
  bool    unique() const { return header_.unique_flag != 0; }
  AttrType key_type() const { return static_cast<AttrType>(header_.key_type); }
  const std::string &file_name() const { return file_name_; }

  /// 把键转成人类可读的形式（测试与 EXPLAIN 输出用）
  std::string key_to_string(const char *key) const;
  /// 把一个值编码成定长键
  std::string value_to_key(const Value &value) const;

  /// 自检：叶子链是否覆盖全部键、顺序是否递增、条数是否与 entry_count 一致
  RC validate(std::vector<std::string> *keys_out = nullptr) const;

  /// 自顶向下递归检查：孩子页号合法、键数在 [min_keys, order] 之间、子树键序列有序
  RC validate_node(PageNum page_num, bool is_root, std::vector<std::string> &keys) const;

private:
  friend class BplusTreeScanner;

  // ---- 键的比较与编码 ----
  int compare_keys(const char *a, const char *b) const;

  // ---- 页内节点的读写 ----
  bool     is_leaf_page(const char *page) const { return page_header(page)->is_leaf != 0; }
  void     init_node_page(char *page, bool is_leaf);
  int32_t  node_key_count(const char *page) const { return page_header(page)->num_key; }
  const char *leaf_key_at(const char *page, int32_t index) const;
  char      *leaf_key_at(char *page, int32_t index);
  const char *leaf_value_at(const char *page, int32_t index) const;
  const char *internal_key_at(const char *page, int32_t index) const;
  char      *internal_key_at(char *page, int32_t index);
  PageNum    child_at(const char *page, int32_t index) const;
  void       set_child_at(char *page, int32_t index, PageNum child) const;

  /// 第一个满足 keys[i] > key 的下标（用于定位孩子与插入位置）
  int32_t upper_bound(const char *page, const char *key) const;
  /// 第一个满足 keys[i] >= key 的下标
  int32_t lower_bound(const char *page, const char *key) const;

  RC find_leaf(const char *key, PageNum *leaf) const;
  RC allocate_node_page(PageNum *page_num, bool is_leaf);

  // ---- 插入 ----
  struct SplitInfo
  {
    bool      split = false;
    PageNum   new_page = INVALID_PAGE_NUM;
    std::string key;  /// 需要向上插入的分隔键
  };
  RC insert_recursive(PageNum page, const char *key, const char *value, SplitInfo *info);
  RC insert_into_leaf(PageNum page, const char *key, const char *value, SplitInfo *info);
  RC insert_into_internal(PageNum page, const char *key, PageNum child, SplitInfo *info);
  RC grow_root(const std::string &key, PageNum left, PageNum right);

  // ---- 删除 ----
  struct DeleteInfo
  {
    bool underflow = false;  /// 该节点的键数是否低于下限，需要由父节点再平衡
  };
  struct PathStep
  {
    PageNum page = INVALID_PAGE_NUM;  /// 祖先节点（内部节点）的页号
    int32_t child_index = 0;          /// 通向下一层的 children 下标
  };
  /// 删除一条索引项：先精确定位条目所在叶子，再沿真实路径自底向上再平衡
  RC locate_entry(PageNum leaf, const char *key, const char *value, PageNum *found_page, int32_t *found_slot);
  RC find_path(PageNum page, PageNum target_leaf, std::vector<PathStep> &path);
  RC erase_from_leaf(PageNum page, int32_t slot, int32_t *num_key_after);
  RC fix_underflow(PageNum parent, int32_t child_index, DeleteInfo *parent_info);
  RC move_key_to_left(PageNum parent, int32_t child_index);
  RC move_key_to_right(PageNum parent, int32_t child_index);
  RC merge_nodes(PageNum parent, int32_t child_index, bool with_left, DeleteInfo *parent_info);
  RC shrink_root();

  RC read_header();
  RC write_header();
  RC update_leaf_link(PageNum page, PageNum next_leaf) const;

  static const IndexNodeHeader *page_header(const char *page)
  {
    return reinterpret_cast<const IndexNodeHeader *>(page);
  }
  static IndexNodeHeader *page_header(char *page)
  {
    return reinterpret_cast<IndexNodeHeader *>(page);
  }

private:
  DiskBufferPool *buffer_pool_ = nullptr;
  std::string     file_name_;
  IndexFileHeader header_;
  bool            opened_ = false;
};
