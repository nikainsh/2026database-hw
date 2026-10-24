# 页级 WAL 设计（PageWAL）

> 目标：在缓冲管理器层提供“先写日志、再写数据页”的崩溃恢复基础。  
> 注意：这是**页级 redo WAL**，不是完整事务日志。

---

## 1. 文件位置与生命周期

- 每个数据文件 `foo.dat` 对应一个 `foo.dat.wal`。
- `create_file` 会删除同路径的旧 WAL，避免新数据文件继承旧日志。
- `open`：
  1. 打开数据文件；
  2. 打开/创建 WAL；
  3. 回放有效记录到数据文件；
  4. fsync 数据文件；
  5. checkpoint：截断 WAL，只保留 WAL header；
  6. 读取数据文件头，校验 free list。
- `close`：flush dirty pages → fsync 数据文件 → checkpoint WAL → 关闭文件。

只读打开时，如果 WAL 里还有记录，返回 `BUFFERPOOL_WAL_ERROR`，因为只读模式无法回放。请先用读写模式打开完成恢复。

---

## 2. WAL 文件格式

### 2.1 WAL header（16 字节，packed）

| 偏移 | 大小 | 字段 | 说明 |
| --- | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4D57414C`（'MWAL'） |
| 4 | 2 | `version` | 1 |
| 6 | 2 | `header_size` | 16 |
| 8 | 4 | `page_size` | 8192 |
| 12 | 4 | `reserved` | 0 |

### 2.2 WAL record（32 字节 header + 8KB 数据，packed）

| 偏移 | 大小 | 字段 | 说明 |
| --- | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4D575243`（'MWRC'） |
| 4 | 4 | `size` | 8192 |
| 8 | 4 | `crc` | 记录 CRC32 |
| 12 | 4 | `reserved` | 0 |
| 16 | 8 | `lsn` | 单调递增日志序号 |
| 24 | 4 | `page_num` | 页号 |
| 28 | 4 | `reserved2` | 0 |
| 32 | 8192 | `data` | 完整页镜像 |

**CRC 计算**：把 record header 的 `crc` 字段置 0，然后对 `header + data` 整体计算 CRC32。

---

## 3. 写路径

```
flush dirty page:
  wal.append_and_sync(page_num, page_data)
      - 写 record
      - fflush
      - fsync(WAL)
  write_page_raw(page_num, page_data)
  frame.clear_dirty()
```

保证：

- 数据页写入磁盘之前，包含该页完整镜像的 WAL 记录已经 fsync。
- 如果进程在写数据页之前/期间崩溃，重启后 WAL 回放可以恢复该页。

---

## 4. 恢复路径

```
open:
  wal.open()
  wal.replay(write_page_raw)
  fsync(data file)
  wal.checkpoint()   // 截断 WAL
  read_header()
  validate_free_list()
```

`replay` 的顺序：

1. 读 WAL header，校验 magic/version/header_size/page_size。
2. 循环读取 record header + data：
   - magic/size/page_num 合法；
   - CRC 匹配；
   - 调用 `write_page_raw(page_num, data)`。
3. 遇到以下情况**停止**，保留此前有效记录：
   - 文件结尾；
   - record header 不完整；
   - data 不完整；
   - magic/size/page_num 非法；
   - CRC 不匹配。
4. 回放完成后，调用方 fsync 数据文件，再 checkpoint 截断 WAL。

---

## 5. Checkpoint

`PageWAL::checkpoint()`：

1. 关闭当前 WAL 文件；
2. 以 `w+b` 重新打开（截断）；
3. 写入 WAL header；
4. fflush + fsync。

**前置条件**：调用方必须已经 fsync 数据文件。当前实现由 `DiskBufferPool::flush_all_buffers()` 保证：

1. flush 所有 dirty frames；
2. 写 header（如果 dirty）；
3. 确认没有残留 dirty frame；
4. fsync 数据文件；
5. checkpoint WAL；
6. `stats.checkpoints++`。

如果还有 dirty frame（例如并发写入或 flush 失败），**不截断 WAL**，避免丢失尚未落盘的页的 redo 记录。

---

## 6. 恢复保证

### 保证

- 在 WAL record fsync 之后、数据页写完成之前的崩溃，可以通过回放恢复该页。
- WAL 文件尾部 torn/corrupt 记录会被忽略，之前的完整记录仍然有效。
- 页 0 header 也会被 WAL 记录，因此 header 更新在 checkpoint 前受保护。

### 不保证

- **事务原子性**：没有 begin/commit/abort 记录，跨多个页的更新可能部分回放。事务语义需要上层事务管理器。
- **undo**：只有 redo，没有回滚。
- **分配元数据的即时持久性**：`allocate_page` / `dispose_page` 修改 header 后，header 只在 flush/checkpoint 时写 WAL/数据文件。若在 header 落盘前崩溃，最新分配/释放可能丢失。已写入的数据页可能变成“孤儿页”，但不会破坏已提交的页内容。
- **torn page 防护**：没有 doublewrite。WAL 回放的是完整页镜像，但数据页本身在写入过程中仍可能 torn；如果崩溃发生在数据页写入且 WAL 记录也丢失（例如 WAL 被外部删除），则无法恢复。
- **数据页 checksum**：目前只有 WAL 记录 CRC 和文件头 CRC，数据页没有 checksum。

---

## 7. 性能特征

- 每次 dirty page flush 都会 append 一条 8KB+32B 记录并 fsync WAL。
- 这是“full-page logging”，写放大明显，但实现简单、恢复直接。
- LSN 目前是“单个 WAL 文件生命周期内”单调递增；checkpoint 截断后继续递增，但进程重启并完成恢复后从 0 重新开始。若需要全局单调 LSN，应由上层事务/日志系统提供。
- 后续优化方向：
  - group commit / WAL buffer；
  - physiological logging（只记录变化字节 + page LSN）；
  - 后台 WAL writer；
  - checkpoint 异步化；
  - doublewrite buffer；
  - 数据页 checksum。
