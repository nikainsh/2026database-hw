# 存储管理器（组件 1）

职责：**按关系模型把数据存到磁盘，并支持高效访问（索引）**。
对应需求表里的组件 (1)，代码在 `src/storage/record`、`src/storage/index`、`src/storage/table`。

依赖方向（只往下依赖，不碰 sql/shell）：

```
sql 算子 / QueryEngine
        │  只用 Table 的公开方法
        ▼
       Db（src/storage/db：catalog.bin、建库建表）
        ▼
      Table ────────────┬─────────────────┐
        │               │                 │
 HeapRecordManager  BplusTreeIndex     TableMeta
 (record/)           (index/)          (table/)
        │               │
        └──── DiskBufferPool v2（src/storage/buffer）──── 文件 / WAL
```

---

## 1. 代码地图

| 文件 | 行数 | 内容 |
| --- | ---: | --- |
| `record/record.h` | 89 | `RID = (page_num, slot_num)`；`Record` 可引用页内内存，也可 `copy_data()` 持有副本 |
| `record/slotted_page.h` | 67 | 页头 `SlottedPageHeader`、槽位读写、`init_slotted_page`、`find_free_slot` |
| `record/record_manager.h` | 66 | 表头 `RecordFileHeader` 与 `HeapRecordManager` 接口 |
| `record/record_manager.cpp` | 305 | 插入（找页 → 找空槽 → 写记录 → 回填 RID）、删除、原地更新、按 RID 读、行数统计 |
| `record/record_scanner.h` | 33 | 全表扫描游标 |
| `record/record_scanner.cpp` | 81 | 按 (页, 槽) 顺序遍历，一次只 pin 一页，跨 4 页做顺序预取 |
| `index/bplus_tree.h` | 208 | 树头 `IndexFileHeader`、节点头 `IndexNodeHeader`、`BplusTree` / `BplusTreeScanner` |
| `index/bplus_tree.cpp` | 1481 | 查找、插入分裂、删除借位/合并/根收缩、范围游标、键编码比较、树级 latch、`validate()` |
| `index/index.h` | 72 | 索引抽象接口 `Index`、`BplusTreeIndex`、文件路径辅助函数 |
| `index/index.cpp` | 100 | 从整条记录里切出被索引列当定长键，包装 `BplusTree` |
| `table/table_meta.h` | 95 | `IndexMeta` / `ColumnMeta` / `TableMeta` 定义 |
| `table/table_meta.cpp` | 291 | 由列定义算偏移与 `record_size`、`make_record` / `get_field_value`、小端序列化 |
| `table/table.h` | 61 | `Table` 接口 |
| `table/table.cpp` | 212 | 组合堆文件与索引：写路径同步索引、唯一冲突回滚、索引列变化时先删后插 |

合计 14 个文件 / 约 3160 行。

---

## 2. 磁盘布局

所有文件按 **8KB 一页**（`BP_PAGE_SIZE`）组织，页号从 0 开始：

```
页 0    缓冲池文件头（v2 里还带 CRC、free list、WAL 元信息）
页 1    表头 RecordFileHeader  /  索引头 IndexFileHeader
页 2+   数据页（slotted page）  /  B+Tree 节点页
```

### 2.1 slotted page

```
+--------------------------- page (8KB) ---------------------------+
| record_count | deleted_count | free_start | free_end |            |  <- 页头
| 记录 0 | 记录 1 | ...            -> 记录区从页头向后生长            |
|                        ...            <- 未使用空间                 |
|  ... | slot 2 | slot 1 | slot 0 |    <- 槽位目录从页尾向前生长       |
+------------------------------------------------------------------+
slot[i] 固定在 BP_PAGE_SIZE - 4*(i+1)，存记录字节偏移，-1 表示已删除
```

- 记录**定长**：`TableMeta::record_size()` 累加各列 `data_len()`，所以 `update` 原地覆写、**RID 不变**。
- 删除只把槽位置 -1，空槽被后续插入复用；一页全空时在下次插入失败时重置成空页。
- 表头页 `RecordFileHeader`：magic `'MDB1'`、`record_size`、有效行数、`last_insert_page`（让连续插入摊还 O(1)）。
  打开时校验 `record_size`，不一致返回 `RC::SCHEMA_FIELD_TYPE_MISMATCH`。

### 2.2 索引文件

- 树头 `IndexFileHeader`：magic `'MIDX'`、键类型/键长、是否唯一、`order`（按页大小算出的阶）、
  `min_keys = order/2`、根页号、最左叶页号、树高、条目数。
- 节点页头 `IndexNodeHeader`：`num_key | is_leaf | next_leaf | prev_leaf`。
  叶子存 `(key, RID)` 且按序串成双向链表；内部节点存 `num_key` 个 key 和 `num_key+1` 个孩子指针。
- 键比较按列类型：`int/bigint/float` 走数值序，`char(n)` 走定长字节序（`'\0'` 补齐）。
- 唯一索引插入重复键返回 `RC::RECORD_DUPLICATE_KEY`，由 `Table` 回滚堆文件里的记录。

文件命名：`<dir>/<table>.heap`、`<dir>/<table>.<index>.index`（`heap_file_path` / `index_file_path`）。

---

## 3. 对外接口（上层只碰这些）

`Table`：

| 方法 | 说明 |
| --- | --- |
| `open(bpm, dir, meta, create)` | 打开/新建堆文件与全部索引 |
| `insert_record(Record&)` | 插入并回填 RID，同步所有索引；唯一冲突 → 回滚记录 |
| `delete_record(RID)` | 先删索引项再删记录 |
| `update_record(RID, Record&)` | 被索引列变了就「删旧项 + 插新项」，冲突则回滚旧值 |
| `get_record(RID, Record&)` | 按 RID 读（返回副本） |
| `start_scan(RecordScanner&)` | 全表扫描游标 |
| `create_index(IndexMeta, replay_data)` | 建索引；`replay_data=true` 时对已有数据重建 |
| `index_by_name` / `index_of_field` | 查索引，优化器选访问路径用 |

`HeapRecordManager` / `BplusTree` 属于内部实现，正常不应该被 sql 层直接调用。

---

## 4. 与缓冲管理器 v2 的约定（重要）

v2 把「取页」拆成了带页锁的两种：

```cpp
pool->get_page_read(page, handle);    // 共享页锁，只能读 const_data()
pool->get_page_write(page, handle);   // 排他页锁，改 value() 后必须 mark_dirty()
```

存储层的用法：

- 只读路径（`get_record`、`RecordScanner::next`、B+Tree 查找/校验）→ `get_page_read`；
- 写路径（插入/删除/更新、B+Tree 分裂合并）→ `get_page_write`；
- **同一线程不能对同一页嵌套取写锁会死锁**（`docs/api.md` 第 6.2 节），存储层的实现已经避免；
- `PageHandle` 是 RAII，析构自动 unpin；活着的 handle 会让对应帧无法淘汰；
- `READ` 句柄调用 `mark_dirty()` 返回 `BUFFERPOOL_READ_ONLY_HANDLE`。

---

## 5. 并发模型

| 层 | 并发控制 |
| --- | --- |
| `BplusTree` | 树级 `std::shared_mutex latch_`：`open/close/insert_entry/delete_entry` 取 `unique_lock`；`get_entry/entry_exists/validate/validate_node` 取 `shared_lock` |
| `BplusTreeScanner` | 生命周期内持树级 `shared_lock`（`tree_lock_`），把结构变更挡在外面；`close()` 释放。**用完必须 `close()` 或 `restart()`/析构**，否则插入会被阻塞 |
| `HeapRecordManager` | 表级 `std::mutex latch_`：`insert/update/delete/close/get_record_count` 串行化。表头（行数、`last_insert_page`）在内存里缓存，写操作只改缓存再落盘，因此不存在「两个线程各自读 count=N 再写回 N+1」的丢更新；`get_record`/扫描走页读锁，不加表锁 |
| `RecordScanner` | 一页只 pin 一次（同页连续取行不再取页），代价是**扫描期间持有当前页读锁**：同一线程里扫描未结束时不要写同一张表，会自死锁 |
| `Table` | 索引与堆文件的更新不是原子事务：多索引写入失败靠 `IndexChangeLog` 逆序回滚；并发写同一张表的原子性仍需上层（执行引擎）保证 |

页级损坏防护：数据页没有 checksum/doublewrite（见 `docs/buffer-v3.md`），所以 `HeapRecordManager` 与
`RecordScanner` 在使用一页之前都会用 `check_slotted_page()` 校验页头（`record_count/deleted_count/free_start/free_end`
互相不越界），读坏页返回 `RC::BUFFERPOOL_CORRUPTED`，而不是按坏偏移去读写。

缓冲池层另有 WAL（`<datafile>.wal` + checkpoint），保证**页级**崩溃恢复；表级/事务级语义不在本组件范围内。

---

## 6. 错误码约定

| 场景 | 返回 |
| --- | --- |
| `record_size` 与表头不一致 | `SCHEMA_FIELD_TYPE_MISMATCH` |
| 页号越界 / 槽位越界 | `RECORD_INVALID_RID` |
| 读写一条已删除的记录 | `RECORD_NOT_EXIST` |
| 唯一索引插入重复键 | `RECORD_DUPLICATE_KEY` |
| 扫描结束 | `RECORD_EOF` |
| 缓冲池层错误（页锁冲突、损坏、WAL 失败） | `BUFFERPOOL_*`（见 `docs/api.md` 第 12 节） |

---

## 7. 已知限制

- **索引**：只支持单列索引、单列主键；没有多列索引，也没有 `drop index`（建了删不掉）。
- **非唯一索引**里同一个键跨多个叶子时，删除只定位并删掉一条 —— 不影响查询正确性，但可能暂时低于最小填充度。
- **记录**：定长，没有变长记录、没有页内压缩整理（`varchar(n)` 也是按定长存）。
- **统计信息**：没有行数/分布统计，优化器只能按「有没有可用索引」选路径，没有 CBO。
- **事务**：没有表级事务、没有 undo/redo；`Table::insert_record` 的「回滚」只是把刚插入的记录删掉。

---

## 8. 测试

`tests/` 下与组件 1 直接相关的：

| 测试 | 覆盖 |
| --- | --- |
| `test_record_manager` | 1000 条插入跨页、按 RID 点读、原地更新、删 1/3、重复删除报错、空槽复用、重开后完整、`record_size` 不匹配报错 |
| `test_bplus_tree` | 与参照模型逐条对比：4000 条含重复键、随机删一半再平衡、四种开闭区间、char/bigint 键、重开持久化、`validate()` 双向自检 |
| `test_concurrent_bplus_tree` | 树级 latch + 页锁下的并发读写 |
| `test_db` | `Table`/`Db` 集成：建表、索引同步、唯一冲突回滚、重建索引、删除同步、重启恢复、`drop` 清文件 |

跑法见仓库根目录的 `build.sh` / `Makefile` / `CMakeLists.txt`。

> Git Bash 里如果 `build.sh` 报一屏 `undefined reference`，先看 `which find`：
> Windows 的 `find.exe` 会抢在 GNU find 前面，导致源码列表为空。脚本已经把 `/usr/bin` 放到
> PATH 最前，若手工改了脚本或换了 shell 需自己确认这一点。
