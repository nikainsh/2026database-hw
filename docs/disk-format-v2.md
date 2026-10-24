# 缓冲管理器磁盘格式 v2

> 适用范围：`DiskBufferPool` 管理的每个数据文件。页大小固定 8KB（`BP_PAGE_SIZE = 8192`）。

---

## 1. 文件布局

```text
page 0         DiskFileHeader（缓冲池文件头）
page 1         upper-layer metadata（记录文件头 / 索引头，缓冲管理器不解释）
page 2..N      data pages
```

- **页 0 由缓冲管理器独占**，上层不能 `get_page(0)`。
- **页 1 继续留给上层**，缓冲管理器只负责分配和读写，不解释内容。
- 每个数据文件旁边有一个 `<datafile>.wal` 页级 redo WAL。

---

## 2. 文件头 `DiskFileHeader` v2

Packed struct，共 36 字节，放在页 0 的开头：

| 偏移 | 大小 | 字段 | 说明 |
| --- | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4D425032`（'MBP2'） |
| 4 | 2 | `version` | 2 |
| 6 | 2 | `header_size` | `sizeof(DiskFileHeader)` = 36 |
| 8 | 4 | `page_size` | 8192 |
| 12 | 4 | `page_count` | 已分配页数，包含页 0 |
| 16 | 4 | `first_free_page` | 空闲页链表头，`-1` 表示空 |
| 20 | 4 | `free_page_count` | 空闲页数量 |
| 24 | 8 | `checkpoint_lsn` | 预留/诊断用，当前写 0 |
| 32 | 4 | `header_crc` | CRC32 |

### 校验

- `header_crc = CRC32(page_0[0..31])`，即不包含 `header_crc` 字段本身。
- 打开时校验：
  - `magic == 0x4D425032`
  - `version == 2`
  - `header_size == 36`
  - `page_size == 8192`
  - `page_count >= 1`
  - `0 <= free_page_count < page_count`
  - `first_free_page == -1` 或 `0 < first_free_page < page_count`
  - `header_crc` 匹配

任何一项失败返回 `RC::BUFFERPOOL_CORRUPTED`。

---

## 3. 空闲页链表

- `header.first_free_page` 指向第一个空闲页。
- 每个空闲页的**前 4 个字节**保存下一个空闲页号（`PageNum`，int32）。
- `header.free_page_count` 是空闲页数量。
- 分配策略：优先复用 free list 头；否则在 `page_count` 处扩展新页。
- 释放策略：必须未 pin、未重复释放；把当前 `first_free_page` 写入页首，再更新 header。

### 打开时校验

`validate_free_list()` 会遍历 free list：

- 页号必须在 `(0, page_count)` 内；
- 不能重复（检测环）；
- 遍历结束后 `free_pages_.size() == free_page_count`。

失败返回 `BUFFERPOOL_CORRUPTED`。

### 重复释放

`dispose_page` 会检查内存中的 `free_pages_` 集合；重复释放返回 `BUFFERPOOL_DOUBLE_FREE`。

---

## 4. 页分配与释放

### `allocate_page`

1. 只读模式返回 `IOERR_ACCESS`。
2. 如果 free list 非空：
   - 读取首页，取出 `next_free`；
   - 把整页清零；
   - 写入零页（经 WAL）；
   - 从 `free_pages_` 移除；
   - 更新 `first_free_page`、`free_page_count`；
   - 返回复用页号。
3. 如果 free list 为空：
   - 在 `page_count` 位置写一个全零页（经 WAL）；
   - `page_count++`；
   - 返回新页号。

上层拿到页号后再 `get_page`。新页保证全零。

### `dispose_page`

1. 只读模式返回 `IOERR_ACCESS`。
2. 页号必须合法且不在 free list。
3. 如果在页表中：
   - 被 pin 返回 `BUFFERPOOL_PAGE_PINNED`；
   - 未 pin 则从页表 detach，frame reset。
4. 写 free list 指针（经 WAL）。
5. 加入 `free_pages_`，更新 header。

被释放页的内容会被丢弃；重新分配时清零。

---

## 5. 与 WAL 的关系

- 所有数据页写入（包括页 0 header、free list 指针、零页）在写数据文件前都会先 append 到 `<datafile>.wal` 并 `fsync`。
- 崩溃恢复时，打开文件会先回放 WAL 中的有效页镜像，再 fsync 数据文件并截断 WAL。
- WAL 记录也包含页 0，因此 header 更新在 checkpoint 前是受 WAL 保护的。
- 分配/释放 header 的持久化时机：header 在 flush/checkpoint 时落盘。若进程在 header 落盘前崩溃，最新的分配元数据可能丢失；这属于当前页级 WAL 的限制，详见 `wal.md`。

---

## 6. 兼容性

- **v1（magic `0x4D425031`）不兼容 v2**。
- 旧文件不能直接被 v2 打开；需要：
  1. 用旧版本导出数据，或
  2. 写一个迁移工具，读取 v1 header/free list 并重写为 v2。
- v2 没有向后兼容的写回；旧代码读不了 v2 文件。

---

## 7. 为什么这样设计

- 页 0 固定 header，保持与旧实现和上层约定兼容（页 1 仍归上层）。
- 显式 packed struct + CRC，避免 `memcpy` 结构体带来的布局/静默损坏问题。
- free list 实现简单、复用直观；缺点是大量空闲页时遍历/校验成本高。后续可改为 bitmap/extent，但需要格式版本升级。
- 不把 checksum 放进每个数据页，是为了不占用上层可用的 8KB 页空间；数据页校验和留作后续 sidecar 方案。
