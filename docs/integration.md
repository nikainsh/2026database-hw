# 合回完整项目：接口对齐清单

本文件给组内合并 v2 缓冲管理器时使用。

---

## 1. 必须一起合并的文件

```text
src/storage/buffer/*              # 整个目录
src/common/rc.h                   # 新增 BUFFERPOOL_* 错误码
```

`src/common/` 其余文件与旧版本兼容。  
`src/storage/buffer/` 内部新增：

```text
checksum.h/.cpp
page_wal.h/.cpp
replacement.h/.cpp
buffer_pool_stats.h
```

---

## 2. 保持不变的契约

以下语义**没有变**，上层不需要改：

- 页 0 是缓冲池文件头，上层不能 `get_page(0)`。
- 页 1 继续留给上层（记录文件头 / 索引头）。
- `allocate_page` 返回全零页。
- `page_count` 包含页 0，`get_page` 接受 `1 <= page <= page_count-1`。
- `dispose_page` 要求页未被 pin，释放后内容丢弃、可复用。
- `mark_dirty` 后页才会在 flush/淘汰时落盘。
- `PageHandle` 析构自动 unpin。
- `flush_all_buffers` / `close` 会刷脏页。

---

## 3. 需要改动的上层调用

### 3.1 多线程访问

默认 `get_page` 现在是**只 pin、不加页锁**的兼容模式。多线程代码建议迁移：

| 场景 | 新 API |
| --- | --- |
| 只读扫描、读取页 | `get_page_read(page, handle)` |
| 修改页内容 | `get_page_write(page, handle)` + `mark_dirty()` |
| 只需要 pin、不需要页锁 | `get_page(page, handle)`（默认 NONE） |

注意：同一线程不要嵌套对同一页调用 `get_page_write`；需要重入时用默认 `get_page` 或重构调用顺序。

### 3.2 close / drop table

关闭文件前必须释放所有 `PageHandle`。  
`DiskBufferPool::close()` 在仍有 handle 时返回 `BUFFERPOOL_HANDLE_BUSY`；`DiskBufferPoolManager::close_file()` 会保留 pool，不会删除映射。

`Db::drop_table` 的顺序应保持：

1. 销毁 `Table` / `RecordManager` / `BPlusTree`；
2. 确保没有扫描器持有 `PageHandle`；
3. `buffer_pool_manager_->close_file(...)`；
4. 删除磁盘文件；
5. 可选删除 `<file>.wal`。

### 3.3 错误码

新增：

```text
BUFFERPOOL_CORRUPTED
BUFFERPOOL_DOUBLE_FREE
BUFFERPOOL_INVALID_FRAME_LIMIT
BUFFERPOOL_HANDLE_BUSY
BUFFERPOOL_WAL_ERROR
BUFFERPOOL_READ_ONLY_HANDLE
BUFFERPOOL_PAGE_FREED
```

上层如果要区分这些错误，按需处理；不处理也不影响原有 `OB_FAIL` 逻辑。

---

## 4. 磁盘格式迁移

v2 文件头 magic 是 `0x4D425032`，v1 是 `0x4D425031`，**不兼容**。

合并后：

- 新建数据库：直接使用 v2。
- 已有 v1 数据文件：需要迁移工具或重新导入。
- 每个数据文件旁边会生成 `<datafile>.wal`；备份/复制数据库时要连同 `.wal` 一起处理，或者先正常 close 让 WAL checkpoint 后再复制。

如果组里希望保留 v1 兼容读取，需要在 `DiskBufferPool::open` 中增加 v1 header 解析和迁移逻辑；当前版本没有做。

---

## 5. 全局帧预算

`DiskBufferPoolManager` 构造参数：

```cpp
DiskBufferPoolManager(int frame_limit_per_file = 32, int total_frame_limit = 4096);
```

`Db` 目前使用：

```cpp
std::make_unique<DiskBufferPoolManager>(32);
```

建议根据机器内存调整 `total_frame_limit`。每个 frame 约 8KB + 元数据；4096 帧约 32MB。

注意：当前淘汰仍是 per-file，全局预算只是“总额上限”，不是真正的全局 frame manager。

---

## 6. 预取

`prefetch_page` / `prefetch_range` 目前是**同步预热**：会阻塞到页读入并 unpin。

建议接入点：

- `RecordScanner::next`：当前页快读完时预取下一页/下 N 页；
- B+Tree 叶子链扫描：预取 `next_leaf`。

示例：

```cpp
buffer_pool->prefetch_range(current_page + 1, 4);
```

真正异步预取（后台线程/队列）留作后续。

---

## 7. 测试合并后要跑

```bash
bash build.sh test
```

除本目录的 6 个测试外，完整项目还应跑：

- `tests/test_record_manager.cpp`
- `tests/test_bplus_tree.cpp`
- `tests/test_db.cpp`
- `tests/run_e2e.sh`

重点验证：

- 记录管理器分配页/读写页；
- B+Tree 分裂/合并/删除页；
- drop table 的 close 顺序；
- 重启后数据/索引恢复；
- WAL 文件生成与 checkpoint。
