/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "storage/index/bplus_tree.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"

static constexpr int NODE_HDR_SIZE = static_cast<int>(sizeof(IndexNodeHeader));

// ---------------------------------------------------------------------------
// 键的编码与比较
// ---------------------------------------------------------------------------

int BplusTree::compare_keys(const char *a, const char *b) const
{
  switch (key_type()) {
    case AttrType::INT: {
      int32_t x = 0, y = 0;
      memcpy(&x, a, sizeof(x));
      memcpy(&y, b, sizeof(y));
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    case AttrType::BIGINT: {
      int64_t x = 0, y = 0;
      memcpy(&x, a, sizeof(x));
      memcpy(&y, b, sizeof(y));
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    case AttrType::FLOAT: {
      float x = 0.0f, y = 0.0f;
      memcpy(&x, a, sizeof(x));
      memcpy(&y, b, sizeof(y));
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    case AttrType::BOOLEAN: {
      int8_t x = 0, y = 0;
      memcpy(&x, a, sizeof(x));
      memcpy(&y, b, sizeof(y));
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    default: return memcmp(a, b, header_.key_len);  /// CHARS：定长且以 '\0' 补齐
  }
}

std::string BplusTree::value_to_key(const Value &value) const
{
  Value casted;
  RC    rc = value.cast_to(key_type(), casted);
  if (OB_FAIL(rc)) {
    return std::string(header_.key_len, '\0');
  }

  std::string key(static_cast<size_t>(header_.key_len), '\0');
  casted.pack_into(&key[0], header_.key_len);
  return key;
}

std::string BplusTree::key_to_string(const char *key) const
{
  Value v;
  v.set_type(key_type());
  v.unpack_from(key, header_.key_len);
  return v.to_string();
}

// ---------------------------------------------------------------------------
// 打开 / 关闭 / 头页
// ---------------------------------------------------------------------------

/// 按页大小算出节点容量：叶子要放 (key,value)，内部节点要放 key + child 指针，取两者较小并保证为偶数
static int compute_order(int key_len)
{
  const int usable = BP_PAGE_SIZE - NODE_HDR_SIZE;
  int       order  = std::min(usable / (key_len + INDEX_VALUE_LEN), (usable - static_cast<int>(sizeof(PageNum))) / (key_len + static_cast<int>(sizeof(PageNum))));
  if (order % 2 != 0) {
    order -= 1;
  }
  return std::max(order, 2);
}

BplusTree::~BplusTree() { close(); }

RC BplusTree::open(DiskBufferPool *buffer_pool, AttrType key_type, int key_len, bool unique, bool create)
{
  if (buffer_pool == nullptr || key_len <= 0 || key_len > BP_PAGE_SIZE / 4) {
    return RC::INVALID_ARGUMENT;
  }

  buffer_pool_ = buffer_pool;
  file_name_   = buffer_pool->file_name();

  if (create) {
    IndexFileHeader header;
    header.key_type     = static_cast<int32_t>(key_type);
    header.key_len      = key_len;
    header.unique_flag  = unique ? 1 : 0;
    header.order        = compute_order(key_len);
    header.min_keys     = header.order / 2;
    header.tree_height  = 1;

    PageNum page_num = INVALID_PAGE_NUM;
    RC      rc       = buffer_pool_->allocate_page(&page_num);  /// 第 1 页：树头
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (page_num != INDEX_HEADER_PAGE) {
      LOG_ERROR("unexpected index header page. file=%s, page=%d", file_name_.c_str(), page_num);
      return RC::INTERNAL;
    }

    PageNum root = INVALID_PAGE_NUM;
    rc          = buffer_pool_->allocate_page(&root);  /// 第 2 页：只有一个空叶子的根
    if (OB_FAIL(rc)) {
      return rc;
    }
    header.root_page       = root;
    header.first_leaf_page = root;

    PageHandle handle;
    rc = buffer_pool_->get_page(root, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    init_node_page(handle.value(), true);
    handle.mark_dirty();

    header_ = header;
    opened_ = true;
    return write_header();
  }

  RC rc = read_header();
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (header_.magic != INDEX_FILE_MAGIC) {
    LOG_ERROR("invalid index file magic. file=%s", file_name_.c_str());
    return RC::IOERR_READ;
  }
  if (header_.key_len != key_len || static_cast<AttrType>(header_.key_type) != key_type) {
    LOG_ERROR("index meta mismatch. file=%s", file_name_.c_str());
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  opened_ = true;
  return RC::SUCCESS;
}

RC BplusTree::close()
{
  opened_      = false;
  buffer_pool_ = nullptr;
  return RC::SUCCESS;
}

RC BplusTree::read_header()
{
  if (buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(INDEX_HEADER_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  IndexFileHeader header;
  memcpy(&header, handle.const_data(), sizeof(header));
  header_ = header;
  return RC::SUCCESS;
}

RC BplusTree::write_header()
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(INDEX_HEADER_PAGE, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  memcpy(handle.value(), &header_, sizeof(header_));
  opened_ = true;
  return handle.mark_dirty();
}

void BplusTree::init_node_page(char *page, bool is_leaf)
{
  memset(page, 0, BP_PAGE_SIZE);
  IndexNodeHeader *header = page_header(page);
  header->num_key    = 0;
  header->is_leaf    = is_leaf ? 1 : 0;
  header->next_leaf  = INVALID_PAGE_NUM;
  header->prev_leaf  = INVALID_PAGE_NUM;
}

RC BplusTree::allocate_node_page(PageNum *page_num, bool is_leaf)
{
  RC rc = buffer_pool_->allocate_page(page_num);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageHandle handle;
  rc        = buffer_pool_->get_page(*page_num, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  init_node_page(handle.value(), is_leaf);
  return handle.mark_dirty();
}

// ---------------------------------------------------------------------------
// 页内访问
// ---------------------------------------------------------------------------

const char *BplusTree::leaf_key_at(const char *page, int32_t index) const
{
  return page + NODE_HDR_SIZE + static_cast<size_t>(index) * (header_.key_len + INDEX_VALUE_LEN);
}

char *BplusTree::leaf_key_at(char *page, int32_t index)
{
  return page + NODE_HDR_SIZE + static_cast<size_t>(index) * (header_.key_len + INDEX_VALUE_LEN);
}

const char *BplusTree::leaf_value_at(const char *page, int32_t index) const
{
  return leaf_key_at(page, index) + header_.key_len;
}

const char *BplusTree::internal_key_at(const char *page, int32_t index) const
{
  return page + NODE_HDR_SIZE + static_cast<size_t>(index) * header_.key_len;
}

char *BplusTree::internal_key_at(char *page, int32_t index)
{
  return page + NODE_HDR_SIZE + static_cast<size_t>(index) * header_.key_len;
}

PageNum BplusTree::child_at(const char *page, int32_t index) const
{
  const int offset = NODE_HDR_SIZE + header_.order * header_.key_len + static_cast<int>(index) * static_cast<int>(sizeof(PageNum));
  PageNum   child  = 0;
  memcpy(&child, page + offset, sizeof(child));
  return child;
}

void BplusTree::set_child_at(char *page, int32_t index, PageNum child) const
{
  const int offset = NODE_HDR_SIZE + header_.order * header_.key_len + static_cast<int>(index) * static_cast<int>(sizeof(PageNum));
  memcpy(page + offset, &child, sizeof(child));
}

int32_t BplusTree::upper_bound(const char *page, const char *key) const
{
  const int32_t count = node_key_count(page);
  if (is_leaf_page(page)) {
    int32_t i = 0;
    while (i < count && compare_keys(leaf_key_at(page, i), key) <= 0) {
      ++i;
    }
    return i;
  }
  int32_t i = 0;
  while (i < count && compare_keys(internal_key_at(page, i), key) <= 0) {
    ++i;
  }
  return i;
}

int32_t BplusTree::lower_bound(const char *page, const char *key) const
{
  const int32_t count = node_key_count(page);
  if (is_leaf_page(page)) {
    int32_t i = 0;
    while (i < count && compare_keys(leaf_key_at(page, i), key) < 0) {
      ++i;
    }
    return i;
  }
  int32_t i = 0;
  while (i < count && compare_keys(internal_key_at(page, i), key) < 0) {
    ++i;
  }
  return i;
}

RC BplusTree::find_leaf(const char *key, PageNum *leaf) const
{
  PageNum page_num = header_.root_page;
  if (page_num == INVALID_PAGE_NUM) {
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }

  while (true) {
    PageHandle handle;
    RC         rc = buffer_pool_->get_page(page_num, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (is_leaf_page(handle.const_data())) {
      *leaf = page_num;
      return RC::SUCCESS;
    }
    const int32_t index = upper_bound(handle.const_data(), key);
    page_num            = child_at(handle.const_data(), index);
  }
}

RC BplusTree::update_leaf_link(PageNum page, PageNum next_leaf) const
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  page_header(handle.value())->next_leaf = next_leaf;
  return handle.mark_dirty();
}

// ---------------------------------------------------------------------------
// 插入
// ---------------------------------------------------------------------------

RC BplusTree::insert_entry(const char *key, const RID &rid)
{
  if (!opened_ || buffer_pool_ == nullptr) {
    return RC::RECORD_OPENNED;
  }

  if (unique()) {
    bool exists = false;
    RC   rc     = entry_exists(key, exists);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (exists) {
      return RC::RECORD_DUPLICATE_KEY;
    }
  }

  char value[INDEX_VALUE_LEN];
  memcpy(value, &rid.page_num, sizeof(rid.page_num));
  memcpy(value + sizeof(rid.page_num), &rid.slot_num, sizeof(rid.slot_num));

  SplitInfo info;
  RC        rc = insert_recursive(header_.root_page, key, value, &info);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (info.split) {
    rc = grow_root(info.key, header_.root_page, info.new_page);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  header_.entry_count += 1;
  return write_header();
}

RC BplusTree::insert_recursive(PageNum page, const char *key, const char *value, SplitInfo *info)
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (is_leaf_page(handle.value())) {
    return insert_into_leaf(page, key, value, info);
  }

  char     *node     = handle.value();
  const int32_t index = upper_bound(node, key);
  const PageNum child = child_at(node, index);

  SplitInfo child_info;
  rc = insert_recursive(child, key, value, &child_info);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (!child_info.split) {
    info->split = false;
    return RC::SUCCESS;
  }

  return insert_into_internal(page, child_info.key.c_str(), child_info.new_page, info);
}

RC BplusTree::insert_into_leaf(PageNum page, const char *key, const char *value, SplitInfo *info)
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *node   = handle.value();
  IndexNodeHeader *header = page_header(node);
  const int32_t    entry_size = header_.key_len + INDEX_VALUE_LEN;
  const int32_t    index      = upper_bound(node, key);

  if (header->num_key < header_.order) {
    /// 后面的条目整体后移一格
    if (index < header->num_key) {
      memmove(node + NODE_HDR_SIZE + (index + 1) * entry_size,
              node + NODE_HDR_SIZE + index * entry_size,
              static_cast<size_t>(header->num_key - index) * entry_size);
    }
    memcpy(leaf_key_at(node, index), key, header_.key_len);
    memcpy(leaf_key_at(node, index) + header_.key_len, value, INDEX_VALUE_LEN);
    header->num_key += 1;
    handle.mark_dirty();

    info->split = false;
    return RC::SUCCESS;
  }

  /// 叶子满了：先在新条目序列里排好，再对半分裂
  const int32_t total       = header_.order + 1;
  const int32_t left_count  = (header_.order + 2) / 2;  /// order 为偶数 -> order/2 + 1
  std::vector<char> buffer(static_cast<size_t>(total) * entry_size);
  if (index > 0) {
    memcpy(buffer.data(), node + NODE_HDR_SIZE, static_cast<size_t>(index) * entry_size);
  }
  memcpy(buffer.data() + static_cast<size_t>(index) * entry_size, key, header_.key_len);
  memcpy(buffer.data() + static_cast<size_t>(index) * entry_size + header_.key_len, value, INDEX_VALUE_LEN);
  if (index < header->num_key) {
    memcpy(buffer.data() + static_cast<size_t>(index + 1) * entry_size,
           node + NODE_HDR_SIZE + static_cast<size_t>(index) * entry_size,
           static_cast<size_t>(header->num_key - index) * entry_size);
  }

  PageNum right_page = INVALID_PAGE_NUM;
  rc                 = allocate_node_page(&right_page, true);
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 右叶子继承原来的后继，并把双向链补好
  const PageNum old_next = header->next_leaf;
  PageHandle    right_handle;
  rc = buffer_pool_->get_page(right_page, right_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  char            *right_node   = right_handle.value();
  IndexNodeHeader *right_header = page_header(right_node);
  right_header->num_key   = total - left_count;
  right_header->next_leaf = old_next;
  right_header->prev_leaf = page;
  memcpy(right_node + NODE_HDR_SIZE, buffer.data() + static_cast<size_t>(left_count) * entry_size,
         static_cast<size_t>(total - left_count) * entry_size);
  right_handle.mark_dirty();

  if (old_next != INVALID_PAGE_NUM) {
    PageHandle next_handle;
    rc = buffer_pool_->get_page(old_next, next_handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    page_header(next_handle.value())->prev_leaf = right_page;
    next_handle.mark_dirty();
  }

  /// 左叶子保留前 left_count 条：必须从临时缓冲整体回写，因为新条目可能插在中间
  memcpy(node + NODE_HDR_SIZE, buffer.data(), static_cast<size_t>(left_count) * entry_size);
  header->num_key   = left_count;
  header->next_leaf = right_page;
  handle.mark_dirty();

  info->split    = true;
  info->new_page = right_page;
  info->key.assign(buffer.data() + static_cast<size_t>(left_count) * entry_size,
                   static_cast<size_t>(header_.key_len));
  return RC::SUCCESS;
}

RC BplusTree::insert_into_internal(PageNum page, const char *key, PageNum child, SplitInfo *info)
{
  PageHandle      handle;
  RC              rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *node   = handle.value();
  IndexNodeHeader *header = page_header(node);
  const int32_t    index  = upper_bound(node, key);

  if (header->num_key < header_.order) {
    if (index < header->num_key) {
      memmove(internal_key_at(node, index + 1), internal_key_at(node, index),
              static_cast<size_t>(header->num_key - index) * header_.key_len);
    }
    memcpy(internal_key_at(node, index), key, header_.key_len);

    for (int32_t i = header->num_key; i > index; --i) {
      set_child_at(node, i + 1, child_at(node, i));
    }
    set_child_at(node, index + 1, child);

    header->num_key += 1;
    handle.mark_dirty();
    info->split = false;
    return RC::SUCCESS;
  }

  /// 内部节点满了：中间那个 key 上移，左右各留一半
  const int32_t total_keys   = header_.order + 1;
  const int32_t median       = header_.order / 2;
  std::vector<char>   keys(static_cast<size_t>(total_keys) * header_.key_len);
  std::vector<PageNum> childs(total_keys + 1);

  if (index > 0) {
    memcpy(keys.data(), internal_key_at(node, 0), static_cast<size_t>(index) * header_.key_len);
    for (int32_t i = 0; i <= index; ++i) {
      childs[i] = child_at(node, i);
    }
  } else {
    childs[0] = child_at(node, 0);
  }
  memcpy(keys.data() + static_cast<size_t>(index) * header_.key_len, key, header_.key_len);
  childs[index + 1] = child;
  if (index < header->num_key) {
    memcpy(keys.data() + static_cast<size_t>(index + 1) * header_.key_len,
           internal_key_at(node, index),
           static_cast<size_t>(header->num_key - index) * header_.key_len);
    for (int32_t i = index + 1; i <= header->num_key; ++i) {
      childs[i + 1] = child_at(node, i);
    }
  }

  PageNum right_page = INVALID_PAGE_NUM;
  rc                 = allocate_node_page(&right_page, false);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageHandle right_handle;
  rc = buffer_pool_->get_page(right_page, right_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  char            *right_node   = right_handle.value();
  IndexNodeHeader *right_header = page_header(right_node);
  right_header->num_key = total_keys - median - 1;
  memcpy(internal_key_at(right_node, 0), keys.data() + static_cast<size_t>(median + 1) * header_.key_len,
         static_cast<size_t>(right_header->num_key) * header_.key_len);
  for (int32_t i = 0; i <= right_header->num_key; ++i) {
    set_child_at(right_node, i, childs[median + 1 + i]);
  }
  right_handle.mark_dirty();

  header->num_key = median;
  memcpy(internal_key_at(node, 0), keys.data(), static_cast<size_t>(median) * header_.key_len);
  for (int32_t i = 0; i <= median; ++i) {
    set_child_at(node, i, childs[i]);
  }
  handle.mark_dirty();

  info->split    = true;
  info->new_page = right_page;
  info->key.assign(keys.data() + static_cast<size_t>(median) * header_.key_len,
                   static_cast<size_t>(header_.key_len));
  return RC::SUCCESS;
}

RC BplusTree::grow_root(const std::string &key, PageNum left, PageNum right)
{
  PageNum root = INVALID_PAGE_NUM;
  RC      rc   = allocate_node_page(&root, false);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageHandle handle;
  rc        = buffer_pool_->get_page(root, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *node   = handle.value();
  IndexNodeHeader *header = page_header(node);
  header->num_key   = 1;
  header->is_leaf   = 0;
  memcpy(internal_key_at(node, 0), key.data(), header_.key_len);
  set_child_at(node, 0, left);
  set_child_at(node, 1, right);
  handle.mark_dirty();

  header_.root_page   = root;
  header_.tree_height += 1;
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 查找
// ---------------------------------------------------------------------------

RC BplusTree::get_entry(const char *key, std::vector<RID> &rids) const
{
  rids.clear();
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }

  PageNum      leaf = INVALID_PAGE_NUM;
  RC           rc   = find_leaf(key, &leaf);
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 重复键可能被分裂打散到相邻的左叶子，所以要沿着 prev_leaf 一直往左找
  PageNum page = leaf;
  while (page != INVALID_PAGE_NUM) {
    PageHandle handle;
    rc        = buffer_pool_->get_page(page, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }

    const char *node  = handle.const_data();
    const int32_t count = node_key_count(node);
    if (count == 0) {
      break;
    }
    /// 这个叶子（以及更左的叶子）的最大键已经小于目标键，不必再走
    if (compare_keys(leaf_key_at(node, count - 1), key) < 0) {
      break;
    }
    if (compare_keys(leaf_key_at(node, 0), key) <= 0) {
      for (int32_t i = 0; i < count; ++i) {
        if (compare_keys(leaf_key_at(node, i), key) != 0) {
          continue;
        }
        PageNum page_num = 0;
        SlotNum slot_num = 0;
        memcpy(&page_num, leaf_value_at(node, i), sizeof(page_num));
        memcpy(&slot_num, leaf_value_at(node, i) + sizeof(page_num), sizeof(slot_num));
        rids.push_back(RID(page_num, slot_num));
      }
    }

    page = page_header(node)->prev_leaf;
  }
  return RC::SUCCESS;
}

RC BplusTree::entry_exists(const char *key, bool &exists) const
{
  std::vector<RID> rids;
  RC               rc = get_entry(key, rids);
  exists              = !rids.empty();
  return rc;
}

// ---------------------------------------------------------------------------
// 删除
// ---------------------------------------------------------------------------

RC BplusTree::delete_entry(const char *key, const RID &rid)
{
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }

  char value[INDEX_VALUE_LEN];
  memcpy(value, &rid.page_num, sizeof(rid.page_num));
  memcpy(value + sizeof(rid.page_num), &rid.slot_num, sizeof(rid.slot_num));

  /// 1) 定位条目真正所在的叶子
  PageNum leaf = INVALID_PAGE_NUM;
  RC      rc   = find_leaf(key, &leaf);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageNum target_page = INVALID_PAGE_NUM;
  int32_t target_slot = -1;
  rc = locate_entry(leaf, key, value, &target_page, &target_slot);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (target_page == INVALID_PAGE_NUM) {
    return RC::RECORD_NOT_EXIST;
  }

  /// 2) 找出根到该叶子的真实路径，才能在正确的下标上做再平衡
  std::vector<PathStep> path;
  if (target_page != header_.root_page) {
    rc = find_path(header_.root_page, target_page, path);
    if (OB_FAIL(rc)) {
      LOG_ERROR("failed to find path to leaf. file=%s, leaf=%d", file_name_.c_str(), target_page);
      return rc;
    }
  }

  /// 3) 删除条目
  int32_t num_key_after = 0;
  rc                    = erase_from_leaf(target_page, target_slot, &num_key_after);
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 4) 自底向上修复低于最小填充度的节点
  bool underflow = num_key_after < header_.min_keys;
  for (int32_t i = static_cast<int32_t>(path.size()) - 1; i >= 0 && underflow; --i) {
    DeleteInfo info;
    rc = fix_underflow(path[i].page, path[i].child_index, &info);
    if (OB_FAIL(rc)) {
      return rc;
    }
    underflow = info.underflow;
  }

  rc = shrink_root();
  if (OB_FAIL(rc)) {
    return rc;
  }

  header_.entry_count -= 1;
  return write_header();
}

/**
 * @brief 定位 (key,value) 这条索引项真正所在的叶子和槽位
 * @details 一个键的重复条目可能被叶子分裂打散到相邻的左叶子，所以从 find_leaf 给的
 *          那个叶子出发，沿 prev_leaf 向左继续找，直到叶子的最大键小于目标键。
 */
RC BplusTree::locate_entry(PageNum leaf, const char *key, const char *value,
                           PageNum *found_page, int32_t *found_slot)
{
  *found_page = INVALID_PAGE_NUM;
  *found_slot = -1;

  PageNum current = leaf;
  while (current != INVALID_PAGE_NUM) {
    PageHandle handle;
    RC         rc = buffer_pool_->get_page(current, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }

    const char          *node   = handle.const_data();
    const IndexNodeHeader *header = page_header(node);

    int32_t index = lower_bound(node, key);
    while (index < header->num_key && compare_keys(leaf_key_at(node, index), key) == 0) {
      if (memcmp(leaf_value_at(node, index), value, INDEX_VALUE_LEN) == 0) {
        *found_page = current;
        *found_slot = index;
        return RC::SUCCESS;
      }
      ++index;
    }

    const int32_t count = header->num_key;
    const PageNum prev  = header->prev_leaf;
    if (count == 0 || prev == INVALID_PAGE_NUM) {
      break;
    }
    /// 左叶子的最大键已经小于目标键，重复项不可能再往左
    if (compare_keys(leaf_key_at(node, count - 1), key) < 0) {
      break;
    }
    current = prev;
  }
  return RC::SUCCESS;
}

/// 深度优先找出根到 target_leaf 的路径（每一层的 children 下标）
RC BplusTree::find_path(PageNum page, PageNum target_leaf, std::vector<PathStep> &path)
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const char *node = handle.const_data();
  if (is_leaf_page(node)) {
    return RC::NOTFOUND;
  }

  const int32_t count = node_key_count(node);
  for (int32_t i = 0; i <= count; ++i) {
    const PageNum child = child_at(node, i);
    if (child == target_leaf) {
      path.push_back(PathStep{page, i});
      return RC::SUCCESS;
    }

    PageHandle child_handle;
    rc = buffer_pool_->get_page(child, child_handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (is_leaf_page(child_handle.const_data())) {
      continue;  /// 是叶子但不是目标叶子
    }

    path.push_back(PathStep{page, i});
    rc = find_path(child, target_leaf, path);
    if (rc == RC::SUCCESS) {
      return RC::SUCCESS;
    }
    path.pop_back();
  }
  return RC::NOTFOUND;
}

/// 从叶子的指定槽位删掉一条索引项
RC BplusTree::erase_from_leaf(PageNum page, int32_t slot, int32_t *num_key_after)
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *node   = handle.value();
  IndexNodeHeader *header = page_header(node);
  if (slot < 0 || slot >= header->num_key) {
    return RC::RECORD_INVALID_RID;
  }

  const int32_t entry_size = header_.key_len + INDEX_VALUE_LEN;
  if (slot + 1 < header->num_key) {
    memmove(node + NODE_HDR_SIZE + slot * entry_size,
            node + NODE_HDR_SIZE + (slot + 1) * entry_size,
            static_cast<size_t>(header->num_key - slot - 1) * entry_size);
  }
  header->num_key -= 1;
  *num_key_after = header->num_key;
  return handle.mark_dirty();
}

RC BplusTree::fix_underflow(PageNum parent, int32_t child_index, DeleteInfo *parent_info)
{
  PageHandle parent_handle;
  RC         rc = buffer_pool_->get_page(parent, parent_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *node   = parent_handle.value();
  const int32_t    count  = node_key_count(node);
  const bool       has_left  = child_index > 0;
  /// 孩子总数 = 键数 + 1，下标范围 0..count，所以右兄弟存在当且仅当 child_index < count
  const bool       has_right = child_index < count;

  LOG_DBG("fix underflow: parent=%d child_index=%d keys=%d min=%d leaf=%d",
          parent, child_index, count, header_.min_keys, is_leaf_page(node) ? 1 : 0);

  if (has_left) {
    const PageNum left = child_at(node, child_index - 1);
    PageHandle    left_handle;
    rc = buffer_pool_->get_page(left, left_handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (node_key_count(left_handle.const_data()) > header_.min_keys) {
      return move_key_to_left(parent, child_index);
    }
  }

  if (has_right) {
    const PageNum right = child_at(node, child_index + 1);
    PageHandle    right_handle;
    rc = buffer_pool_->get_page(right, right_handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (node_key_count(right_handle.const_data()) > header_.min_keys) {
      return move_key_to_right(parent, child_index);
    }
  }

  if (has_left) {
    return merge_nodes(parent, child_index, true, parent_info);
  }
  if (has_right) {
    return merge_nodes(parent, child_index, false, parent_info);
  }

  /// 没有兄弟，说明这是根，根允许低于下限
  parent_info->underflow = count < header_.min_keys;
  return RC::SUCCESS;
}

RC BplusTree::move_key_to_left(PageNum parent, int32_t child_index)
{
  PageHandle parent_handle;
  RC         rc = buffer_pool_->get_page(parent, parent_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  char *parent_node = parent_handle.value();

  const PageNum left  = child_at(parent_node, child_index - 1);
  const PageNum child = child_at(parent_node, child_index);

  PageHandle left_handle, child_handle;
  rc = buffer_pool_->get_page(left, left_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = buffer_pool_->get_page(child, child_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *left_node   = left_handle.value();
  char            *child_node  = child_handle.value();
  IndexNodeHeader *left_header = page_header(left_node);
  IndexNodeHeader *child_hdr   = page_header(child_node);

  if (is_leaf_page(left_node)) {
    /// 左兄弟的最后一条记录搬到孩子开头，父键更新为孩子的最小键
    const int32_t entry_size = header_.key_len + INDEX_VALUE_LEN;
    const int32_t take       = left_header->num_key - 1;
    memmove(child_node + NODE_HDR_SIZE + entry_size, child_node + NODE_HDR_SIZE,
            static_cast<size_t>(child_hdr->num_key) * entry_size);
    memcpy(child_node + NODE_HDR_SIZE, left_node + NODE_HDR_SIZE + take * entry_size, entry_size);
    child_hdr->num_key += 1;
    left_header->num_key -= 1;

    memcpy(internal_key_at(parent_node, child_index - 1), leaf_key_at(child_node, 0), header_.key_len);
  } else {
    /// 父键下沉到孩子开头，左兄弟的最后一个孩子指针跟着搬过来
    const int32_t take = left_header->num_key - 1;
    memmove(internal_key_at(child_node, 1), internal_key_at(child_node, 0),
            static_cast<size_t>(child_hdr->num_key) * header_.key_len);
    memcpy(internal_key_at(child_node, 0), internal_key_at(parent_node, child_index - 1), header_.key_len);

    for (int32_t i = child_hdr->num_key + 1; i > 0; --i) {
      set_child_at(child_node, i, child_at(child_node, i - 1));
    }
    /// 左兄弟的第 take+1 个孩子（它最右边那个）划给右兄弟
    set_child_at(child_node, 0, child_at(left_node, take + 1));

    memcpy(internal_key_at(parent_node, child_index - 1), internal_key_at(left_node, take), header_.key_len);

    child_hdr->num_key += 1;
    left_header->num_key -= 1;
  }

  left_handle.mark_dirty();
  child_handle.mark_dirty();
  return parent_handle.mark_dirty();
}

RC BplusTree::move_key_to_right(PageNum parent, int32_t child_index)
{
  PageHandle parent_handle;
  RC         rc = buffer_pool_->get_page(parent, parent_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  char *parent_node = parent_handle.value();

  const PageNum child  = child_at(parent_node, child_index);
  const PageNum right  = child_at(parent_node, child_index + 1);

  PageHandle child_handle, right_handle;
  rc = buffer_pool_->get_page(child, child_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = buffer_pool_->get_page(right, right_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *child_node  = child_handle.value();
  char            *right_node  = right_handle.value();
  IndexNodeHeader *child_hdr   = page_header(child_node);
  IndexNodeHeader *right_hdr   = page_header(right_node);

  if (is_leaf_page(child_node)) {
    /// 右兄弟的第一条记录接到孩子尾部，父键更新为右兄弟的新最小键
    memcpy(leaf_key_at(child_node, child_hdr->num_key), leaf_key_at(right_node, 0),
           static_cast<size_t>(header_.key_len + INDEX_VALUE_LEN));
    child_hdr->num_key += 1;

    const int32_t entry_size = header_.key_len + INDEX_VALUE_LEN;
    memmove(right_node + NODE_HDR_SIZE, right_node + NODE_HDR_SIZE + entry_size,
            static_cast<size_t>(right_hdr->num_key - 1) * entry_size);
    right_hdr->num_key -= 1;

    memcpy(internal_key_at(parent_node, child_index), leaf_key_at(right_node, 0), header_.key_len);
  } else {
    memcpy(internal_key_at(child_node, child_hdr->num_key), internal_key_at(parent_node, child_index),
           static_cast<size_t>(header_.key_len));
    set_child_at(child_node, child_hdr->num_key + 1, child_at(right_node, 0));
    child_hdr->num_key += 1;

    memmove(internal_key_at(right_node, 0), internal_key_at(right_node, 1),
            static_cast<size_t>(right_hdr->num_key - 1) * header_.key_len);
    for (int32_t i = 0; i < right_hdr->num_key; ++i) {
      set_child_at(right_node, i, child_at(right_node, i + 1));
    }
    right_hdr->num_key -= 1;

    memcpy(internal_key_at(parent_node, child_index), internal_key_at(right_node, 0), header_.key_len);
  }

  child_handle.mark_dirty();
  right_handle.mark_dirty();
  return parent_handle.mark_dirty();
}

RC BplusTree::merge_nodes(PageNum parent, int32_t child_index, bool with_left, DeleteInfo *parent_info)
{
  PageHandle parent_handle;
  RC         rc = buffer_pool_->get_page(parent, parent_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  char *parent_node = parent_handle.value();

  /// 合并后消失的那个父键下标
  const int32_t key_index   = with_left ? child_index - 1 : child_index;
  const PageNum keep        = child_at(parent_node, key_index);
  const PageNum give_up     = child_at(parent_node, key_index + 1);

  PageHandle keep_handle, give_handle;
  rc = buffer_pool_->get_page(keep, keep_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = buffer_pool_->get_page(give_up, give_handle);
  if (OB_FAIL(rc)) {
    return rc;
  }

  char            *keep_node  = keep_handle.value();
  char            *give_node  = give_handle.value();
  IndexNodeHeader *keep_hdr   = page_header(keep_node);
  IndexNodeHeader *give_hdr   = page_header(give_node);

  LOG_DBG("merge: parent=%d key_index=%d with_left=%d keep=%d(%d keys) give=%d(%d keys) leaf=%d",
          parent, key_index, with_left ? 1 : 0, keep, keep_hdr->num_key, give_up, give_hdr->num_key,
          is_leaf_page(keep_node) ? 1 : 0);

  if (is_leaf_page(keep_node)) {
    const int32_t entry_size = header_.key_len + INDEX_VALUE_LEN;
    memcpy(keep_node + NODE_HDR_SIZE + static_cast<size_t>(keep_hdr->num_key) * entry_size,
           give_node + NODE_HDR_SIZE,
           static_cast<size_t>(give_hdr->num_key) * entry_size);
    keep_hdr->num_key += give_hdr->num_key;

    keep_hdr->next_leaf = give_hdr->next_leaf;
    if (give_hdr->next_leaf != INVALID_PAGE_NUM) {
      PageHandle next_handle;
      rc = buffer_pool_->get_page(give_hdr->next_leaf, next_handle);
      if (OB_FAIL(rc)) {
        return rc;
      }
      page_header(next_handle.value())->prev_leaf = keep;
      next_handle.mark_dirty();
    }
  } else {
    /// 中间把父键搬下来，再接上右节点的全部 key 与孩子
    memcpy(internal_key_at(keep_node, keep_hdr->num_key), internal_key_at(parent_node, key_index),
           static_cast<size_t>(header_.key_len));
    keep_hdr->num_key += 1;

    memcpy(internal_key_at(keep_node, keep_hdr->num_key), give_node + NODE_HDR_SIZE,
           static_cast<size_t>(give_hdr->num_key) * header_.key_len);
    keep_hdr->num_key += give_hdr->num_key;

    for (int32_t i = 0; i <= give_hdr->num_key; ++i) {
      set_child_at(keep_node, keep_hdr->num_key - give_hdr->num_key + i, child_at(give_node, i));
    }
  }

  keep_handle.mark_dirty();
  give_handle.reset();

  rc = buffer_pool_->dispose_page(give_up);
  if (OB_FAIL(rc)) {
    return rc;
  }

  /// 父节点少了一个键和一个孩子：被删掉的孩子是 key_index+1，后面的孩子依次前移
  const int32_t parent_count = page_header(parent_node)->num_key;
  if (key_index + 1 < parent_count) {
    memmove(internal_key_at(parent_node, key_index), internal_key_at(parent_node, key_index + 1),
            static_cast<size_t>(parent_count - key_index - 1) * header_.key_len);
  }
  for (int32_t i = key_index + 2; i <= parent_count; ++i) {
    set_child_at(parent_node, i - 1, child_at(parent_node, i));
  }
  page_header(parent_node)->num_key = parent_count - 1;
  parent_handle.mark_dirty();

  parent_info->underflow = (parent_count - 1) < header_.min_keys;
  return RC::SUCCESS;
}

RC BplusTree::shrink_root()
{
  PageHandle handle;
  RC         rc = buffer_pool_->get_page(header_.root_page, handle);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (is_leaf_page(handle.value()) || page_header(handle.const_data())->num_key > 0) {
    return RC::SUCCESS;
  }

  const PageNum old_root = header_.root_page;
  const PageNum new_root = child_at(handle.const_data(), 0);
  handle.reset();

  LOG_DBG("shrink root: old=%d new=%d height=%d", old_root, new_root, header_.tree_height);

  rc = buffer_pool_->dispose_page(old_root);
  if (OB_FAIL(rc)) {
    return rc;
  }

  header_.root_page    = new_root;
  header_.tree_height -= 1;
  return RC::SUCCESS;
}

// ---------------------------------------------------------------------------
// 扫描与自检
// ---------------------------------------------------------------------------

RC BplusTree::make_scanner(BplusTreeScanner &scanner,
                           const char *left_key, bool left_inclusive,
                           const char *right_key, bool right_inclusive) const
{
  if (!opened_) {
    return RC::RECORD_OPENNED;
  }
  return scanner.init(const_cast<BplusTree *>(this),
                      left_key, left_inclusive, right_key, right_inclusive);
}

RC BplusTreeScanner::init(BplusTree *tree,
                          const char *left_key, bool left_inclusive,
                          const char *right_key, bool right_inclusive)
{
  if (tree == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  tree_             = tree;
  left_inclusive_   = left_inclusive;
  right_inclusive_  = right_inclusive;
  finished_         = false;
  handle_.reset();

  if (tree_->header_.root_page == INVALID_PAGE_NUM) {
    return RC::RECORD_EOF;
  }

  /// 无下界时从最左叶子开始，否则定位到包含该键的叶子
  if (left_key == nullptr) {
    current_page_ = tree_->header_.first_leaf_page;
    current_slot_ = 0;
    left_key_.clear();
  } else {
    left_key_.assign(left_key, tree_->header_.key_len);
    RC rc = tree_->find_leaf(left_key, &current_page_);
    if (OB_FAIL(rc)) {
      return rc;
    }
    current_slot_ = 0;
  }

  right_key_.clear();
  if (right_key != nullptr) {
    right_key_.assign(right_key, tree_->header_.key_len);
  }
  return RC::SUCCESS;
}

RC BplusTreeScanner::restart()
{
  handle_.reset();
  finished_     = false;
  current_slot_ = 0;
  if (!left_key_.empty()) {
    return tree_->find_leaf(left_key_.c_str(), &current_page_);
  }
  current_page_ = tree_->header_.first_leaf_page;
  return RC::SUCCESS;
}

RC BplusTreeScanner::next(std::string &key, RID &rid)
{
  while (!finished_) {
    if (!handle_.valid()) {
      if (current_page_ == INVALID_PAGE_NUM) {
        finished_ = true;
        break;
      }
      RC rc = tree_->buffer_pool_->get_page(current_page_, handle_);
      if (OB_FAIL(rc)) {
        return rc;
      }
      current_slot_ = 0;
    }

    const char *page = handle_.const_data();

    while (current_slot_ < tree_->node_key_count(page)) {
      const char *this_key  = tree_->leaf_key_at(page, current_slot_);
      const int   cmp_left  = left_key_.empty() ? 1 : tree_->compare_keys(this_key, left_key_.c_str());
      const bool  pass_left = left_key_.empty() ? true : (left_inclusive_ ? cmp_left >= 0 : cmp_left > 0);

      if (!pass_left) {
        ++current_slot_;
        continue;
      }

      if (!right_key_.empty()) {
        const int cmp_right = tree_->compare_keys(this_key, right_key_.c_str());
        const bool in_range = right_inclusive_ ? cmp_right <= 0 : cmp_right < 0;
        if (!in_range) {
          finished_ = true;
          break;
        }
      }

      key.assign(this_key, static_cast<size_t>(tree_->header_.key_len));
      memcpy(&rid.page_num, tree_->leaf_value_at(page, current_slot_), sizeof(rid.page_num));
      memcpy(&rid.slot_num,
             tree_->leaf_value_at(page, current_slot_) + sizeof(rid.page_num),
             sizeof(rid.slot_num));
      ++current_slot_;
      return RC::SUCCESS;
    }

    if (finished_) {
      break;
    }
    RC rc = move_to_next_leaf();
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  return RC::RECORD_EOF;
}

RC BplusTreeScanner::move_to_next_leaf()
{
  const char *page    = handle_.const_data();
  const PageNum next  = BplusTree::page_header(page)->next_leaf;
  handle_.reset();
  current_page_ = next;
  current_slot_ = 0;
  return RC::SUCCESS;
}

RC BplusTree::validate_node(PageNum page_num, bool is_root, std::vector<std::string> &keys) const
{
  if (page_num < INDEX_FIRST_NODE_PAGE || page_num >= buffer_pool_->page_count()) {
    LOG_ERROR("child pointer out of range. file=%s, page=%s",
              file_name_.c_str(), std::to_string(page_num).c_str());
    return RC::INTERNAL;
  }

  PageHandle handle;
  RC         rc = buffer_pool_->get_page(page_num, handle);
  if (OB_FAIL(rc)) {
    LOG_ERROR("failed to read node page. file=%s, page=%s, rc=%s",
              file_name_.c_str(), std::to_string(page_num).c_str(), strrc(rc));
    return rc;
  }

  const char *node  = handle.const_data();
  const int32_t count = node_key_count(node);
  if (count > header_.order) {
    LOG_ERROR("node overflow. file=%s, page=%s, count=%s, order=%d", file_name_.c_str(),
              std::to_string(page_num).c_str(), std::to_string(count).c_str(), header_.order);
    return RC::INTERNAL;
  }
  /// 除根之外，任何节点的键数都不能低于下限
  if (!is_root && count < (is_leaf_page(node) ? header_.min_keys : header_.min_keys)) {
    LOG_ERROR("node underflow. file=%s, page=%s, count=%s, min=%d", file_name_.c_str(),
              std::to_string(page_num).c_str(), std::to_string(count).c_str(), header_.min_keys);
    return RC::INTERNAL;
  }

  if (is_leaf_page(node)) {
    for (int32_t i = 0; i < count; ++i) {
      std::string key;
      key.assign(leaf_key_at(node, i), static_cast<size_t>(header_.key_len));
      keys.push_back(key);
    }
    return RC::SUCCESS;
  }

  if (count == 0) {
    LOG_ERROR("internal node without key. file=%s, page=%s", file_name_.c_str(), std::to_string(page_num).c_str());
    return RC::INTERNAL;
  }

  for (int32_t i = 0; i <= count; ++i) {
    rc = validate_node(child_at(node, i), false, keys);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC BplusTree::validate(std::vector<std::string> *keys_out) const
{
  std::vector<std::string> keys;
  if (keys_out != nullptr) {
    keys_out->clear();
  }

  /// 1) 叶子链上的键必须单调不减，条目总数与 entry_count 一致，双向链完整
  int64_t     total    = 0;
  PageNum     page_num = header_.first_leaf_page;
  std::string prev_key;
  bool        has_prev = false;
  while (page_num != INVALID_PAGE_NUM) {
    PageHandle handle;
    RC         rc = buffer_pool_->get_page(page_num, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!is_leaf_page(handle.const_data())) {
      LOG_ERROR("leaf chain contains an internal node. file=%s, page=%d", file_name_.c_str(), page_num);
      return RC::INTERNAL;
    }

    const int32_t count = node_key_count(handle.const_data());
    if (count > header_.order) {
      LOG_ERROR("leaf overflow. file=%s, page=%d, count=%d", file_name_.c_str(), page_num, count);
      return RC::INTERNAL;
    }

    for (int32_t i = 0; i < count; ++i) {
      std::string key;
      key.assign(leaf_key_at(handle.const_data(), i), static_cast<size_t>(header_.key_len));
      if (has_prev && compare_keys(prev_key.c_str(), key.c_str()) > 0) {
        LOG_ERROR("leaf keys out of order. file=%s", file_name_.c_str());
        return RC::INTERNAL;
      }
      prev_key = key;
      has_prev = true;
      ++total;
      keys.push_back(key);
    }

    const PageNum next = page_header(handle.const_data())->next_leaf;
    if (next != INVALID_PAGE_NUM) {
      PageHandle next_handle;
      rc = buffer_pool_->get_page(next, next_handle);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (page_header(next_handle.const_data())->prev_leaf != page_num) {
        LOG_ERROR("broken backward link. file=%s, page=%d", file_name_.c_str(), page_num);
        return RC::INTERNAL;
      }
    }
    page_num = next;
  }

  if (total != header_.entry_count) {
    LOG_ERROR("entry count mismatch. file=%s, chain=%s, header=%s",
              file_name_.c_str(), std::to_string(total).c_str(), std::to_string(header_.entry_count).c_str());
    return RC::INTERNAL;
  }

  /// 2) 自顶向下再走一遍：路径上的键序列必须与叶子链完全一致
  std::vector<std::string> from_root;
  RC                       rc = validate_node(header_.root_page, true, from_root);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (from_root.size() != keys.size()) {
    LOG_ERROR("root path and leaf chain disagree. file=%s, root=%s, chain=%s", file_name_.c_str(),
              std::to_string(from_root.size()).c_str(), std::to_string(keys.size()).c_str());
    return RC::INTERNAL;
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    if (from_root[i] != keys[i]) {
      LOG_ERROR("root path differs from leaf chain at %s. file=%s", std::to_string(i).c_str(), file_name_.c_str());
      return RC::INTERNAL;
    }
  }

  /// 3) 链上的每个键都必须能查到
  for (const std::string &key : keys) {
    PageNum leaf = INVALID_PAGE_NUM;
    rc           = find_leaf(key.c_str(), &leaf);
    if (OB_FAIL(rc)) {
      return rc;
    }

    bool found = false;
    rc         = entry_exists(key.c_str(), found);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (!found) {
      LOG_ERROR("key not reachable from root. file=%s", file_name_.c_str());
      return RC::INTERNAL;
    }
  }

  if (keys_out != nullptr) {
    *keys_out = keys;
  }
  return RC::SUCCESS;
}
