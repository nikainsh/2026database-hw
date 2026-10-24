# 缓冲管理器接口使用说明（v2）

头文件：

```cpp
#include "storage/buffer/disk_buffer_pool.h"
```

---

## 1. 错误码

`common/rc.h` 新增：

```cpp
BUFFERPOOL_CORRUPTED
BUFFERPOOL_DOUBLE_FREE
BUFFERPOOL_INVALID_FRAME_LIMIT
BUFFERPOOL_HANDLE_BUSY
BUFFERPOOL_WAL_ERROR
BUFFERPOOL_READ_ONLY_HANDLE
BUFFERPOOL_PAGE_FREED
```

沿用：

```cpp
BUFFERPOOL_OPEN
BUFFERPOOL_NOBUF
BUFFERPOOL_INVALID_PAGE_NUM
BUFFERPOOL_PAGE_PINNED
IOERR_ACCESS
FILE_EXIST
FILE_OPEN
IOERR_READ / IOERR_WRITE / IOERR_SEEK / IOERR_SYNC
```

判断方式不变：

```cpp
RC rc = ...;
if (OB_FAIL(rc)) { ... }
```

---

## 2. 基本生命周期

### 2.1 通过 Manager 管理多个文件（推荐）

```cpp
DiskBufferPoolManager bpm(
    32,    // 每个文件最多 32 帧
    4096   // 所有文件总共最多 4096 帧
);

// 新建文件
RC rc = bpm.create_file("data/demo/student.heap");
if (OB_FAIL(rc) && rc != RC::FILE_EXIST) { ... }

// 打开
DiskBufferPool *pool = nullptr;
rc = bpm.open_file("data/demo/student.heap", ReadWriteMode::READ_WRITE, &pool);
if (OB_FAIL(rc)) { ... }

// ... 使用 pool ...

// 关闭单个文件
rc = bpm.close_file("data/demo/student.heap");

// 关闭全部
rc = bpm.close();
```

注意：

- 路径会被 `std::filesystem::absolute(...).lexically_normal()` 规范化；
- 同一路径只会打开一份；
- 打开时如果已有只读 pool，再请求 READ_WRITE 会返回 `IOERR_ACCESS`；
- `close_file` / `close` 在仍有未释放 `PageHandle` 时返回 `BUFFERPOOL_HANDLE_BUSY`，并保留 pool。

### 2.2 直接使用 DiskBufferPool

```cpp
DiskBufferPool pool;
RC rc = pool.create_file("data/demo/student.heap");
if (OB_FAIL(rc) && rc != RC::FILE_EXIST) { ... }

rc = pool.open("data/demo/student.heap", ReadWriteMode::READ_WRITE, 32, true);
if (OB_FAIL(rc)) { ... }

rc = pool.close();
```

`open` 的最后一个参数是 `enable_wal`，默认 `true`。

---

## 3. 页分配与访问

### 3.1 分配新页

```cpp
PageNum page = INVALID_PAGE_NUM;
RC rc = pool->allocate_page(&page);
if (OB_FAIL(rc)) { ... }
```

- 返回的页已经清零；
- 页号从 1 开始（页 0 是缓冲池 header）；
- 页 1 通常由上层用作记录/索引 header；
- 新页不会自动 pin，需要接着 `get_page_write`。

### 3.2 写页

```cpp
PageHandle handle;
RC rc = pool->get_page_write(page, handle);
if (OB_FAIL(rc)) { ... }

// handle.value() 返回 8KB 缓冲区
memcpy(handle.value(), data, BP_PAGE_SIZE);

// 必须 mark_dirty，否则不会在 flush/淘汰时写回
rc = handle.mark_dirty();
handle.reset();   // 或让 handle 析构
```

### 3.3 读页

```cpp
PageHandle handle;
RC rc = pool->get_page_read(page, handle);
if (OB_FAIL(rc)) { ... }

const char *data = handle.const_data();
// 只读使用；不要通过 value() 修改，也不要 mark_dirty

handle.reset();
```

### 3.4 兼容模式（默认 `get_page`）

```cpp
PageHandle handle;
pool->get_page(page, handle);   // PageAccess::NONE
```

- 只 pin，不加页锁；
- 旧代码可以直接使用；
- 多线程下如果多个线程读写同一页，不安全；
- `mark_dirty()` 在 NONE 下允许（兼容旧代码）。

### 3.5 `PageAccess`

```cpp
enum class PageAccess
{
  NONE,   // pin only，兼容模式
  READ,   // shared page latch
  WRITE,  // exclusive page latch
};
```

显式指定：

```cpp
pool->get_page(page, handle, PageAccess::READ);
pool->get_page(page, handle, PageAccess::WRITE);
```

---

## 4. `PageHandle` 规则

```cpp
PageHandle handle;
pool->get_page_write(page, handle);

handle.valid();       // bool
handle.page_id();     // PageNum
handle.frame_id();    // FrameId
handle.access();      // PageAccess
handle.value();       // char*（可写）
handle.const_data();  // const char*（只读）
handle.mark_dirty();  // 仅 WRITE / NONE
handle.reset();       // 释放页锁 + unpin
```

规则：

1. `PageHandle` 活着时，对应 frame 不会被淘汰。
2. 析构自动 reset；不要手动 `unpin_page`。
3. `READ` 句柄调用 `mark_dirty` 返回 `BUFFERPOOL_READ_ONLY_HANDLE`。
4. 不要把 `value()` 返回的指针保存到 handle 生命周期之外。
5. 不要把 `PageHandle` 在持有期间跨线程移动（它本身没有线程绑定，但页锁语义由使用者保证）。
6. 关闭 pool 前必须释放所有 handle。

---

## 5. 释放页

```cpp
RC rc = pool->dispose_page(page);
```

- 页必须未被 pin；
- 页不能已经在 free list 中（否则 `BUFFERPOOL_DOUBLE_FREE`）；
- 释放后内容丢弃，后续分配会清零；
- 释放页会进入 free list，优先被复用。

---

## 6. 多线程用法

### 6.1 读多写少

```cpp
// reader
PageHandle h;
if (OB_SUCC(pool->get_page_read(page, h))) {
  // 读取 h.const_data()
}

// writer
PageHandle h;
if (OB_SUCC(pool->get_page_write(page, h))) {
  // 修改 h.value()
  h.mark_dirty();
}
```

### 6.2 避免同一线程重入同一页写锁

```cpp
// 错误：同一线程已经持有 page 的 WRITE 锁，再次 get_page_write 会死锁
PageHandle a, b;
pool->get_page_write(page, a);
pool->get_page_write(page, b);   // deadlock

// 正确 1：先释放 a
a.reset();
pool->get_page_write(page, b);

// 正确 2：只需要 pin 时用兼容模式
pool->get_page(page, b);         // NONE，不加页锁
```

### 6.3 关闭前释放 handle

```cpp
{
  PageHandle h;
  pool->get_page_read(page, h);
}   // h 析构，自动释放

RC rc = bpm.close_file(path);   // 现在可以成功
```

如果 `close` 返回 `BUFFERPOOL_HANDLE_BUSY`，说明还有 handle 没释放。

---

## 7. 只读模式

```cpp
DiskBufferPool *pool = nullptr;
bpm.open_file(path, ReadWriteMode::READ_ONLY, &pool);

// 读：OK
PageHandle h;
pool->get_page_read(page, h);

// 写：拒绝
pool->allocate_page(&new_page);   // IOERR_ACCESS
pool->dispose_page(page);         // IOERR_ACCESS
h.mark_dirty();                   // 如果 h 是 READ：BUFFERPOOL_READ_ONLY_HANDLE
```

只读打开时，如果 WAL 里还有未恢复记录，返回 `BUFFERPOOL_WAL_ERROR`。请先用 READ_WRITE 打开完成恢复。

---

## 8. Flush / Checkpoint / WAL

```cpp
pool->flush_page(page);        // 刷单个已缓存脏页；未缓存时 no-op
pool->flush_all_buffers();     // 刷全部脏页 + fsync 数据 + checkpoint WAL
pool->checkpoint();            // 同上
```

行为：

- `flush_page` 只刷缓存中的页；如果页不在缓存里，不会主动读进来。
- `flush_all_buffers` 会：
  1. 收集 dirty frames 并 pin；
  2. 逐页写 WAL + 数据页；
  3. 写 header（如果 dirty）；
  4. 如果还有 dirty frame，不截断 WAL；
  5. fsync 数据文件；
  6. checkpoint 截断 WAL。
- `close` 内部会调用 flush/checkpoint。

WAL 文件：

```text
<datafile>.wal
```

数据文件 + WAL 一起构成崩溃恢复单元。备份/复制时要一起处理。

---

## 9. 统计

```cpp
BufferPoolStatsSnapshot s = pool->stats();
printf("hits=%llu misses=%llu evictions=%llu\n",
       (unsigned long long)s.hits,
       (unsigned long long)s.misses,
       (unsigned long long)s.evictions);
```

字段：

```text
hits, misses, evictions, dirty_evictions,
reads, writes, wal_writes, wal_syncs,
prefetches, no_buffer, handle_busy,
double_free, corrupted, flushes, checkpoints
```

---

## 10. 预取

```cpp
pool->prefetch_page(page);
pool->prefetch_range(start_page, count);
```

当前是**同步预热**：

- 读入页并立即 unpin；
- 不会返回 `PageHandle`；
- 会阻塞到 I/O 完成；
- 适合扫描器在顺序读取前提前调用。

后续可替换为后台预取线程/队列。

---

## 11. Manager 全局帧预算

```cpp
DiskBufferPoolManager bpm(32, 4096);
```

- 每文件最多 32 帧；
- 所有打开文件合计最多 4096 帧；
- 打开新文件时，实际帧数 = `min(32, 剩余全局帧数)`；
- 剩余不足 1 帧时返回 `BUFFERPOOL_NOBUF`；
- 淘汰仍是 per-file；真正全局 frame manager 是后续工作。

查询：

```cpp
bpm.total_frame_limit();
bpm.used_frames();
bpm.remaining_frames();
```

---

## 12. 常见错误处理

| 返回值 | 含义 / 处理 |
| --- | --- |
| `BUFFERPOOL_HANDLE_BUSY` | 还有 PageHandle 没释放；先 reset/析构所有 handle |
| `BUFFERPOOL_INVALID_FRAME_LIMIT` | `frame_limit <= 0` 或过大 |
| `BUFFERPOOL_DOUBLE_FREE` | 同一页被释放两次；检查上层逻辑 |
| `BUFFERPOOL_PAGE_FREED` | 访问已释放页；检查 RID/页号是否失效 |
| `BUFFERPOOL_CORRUPTED` | header CRC / free list / 短读等；文件可能损坏 |
| `BUFFERPOOL_WAL_ERROR` | WAL 打开/回放/只读恢复失败 |
| `BUFFERPOOL_NOBUF` | 所有帧都被 pin 或全局预算耗尽 |
| `BUFFERPOOL_PAGE_PINNED` | 释放被 pin 的页；先释放 handle |

---

## 13. 最小完整示例

```cpp
#include "storage/buffer/disk_buffer_pool.h"

RC demo()
{
  DiskBufferPoolManager bpm(32, 4096);

  RC rc = bpm.create_file("demo.heap");
  if (OB_FAIL(rc) && rc != RC::FILE_EXIST) {
    return rc;
  }

  DiskBufferPool *pool = nullptr;
  rc = bpm.open_file("demo.heap", ReadWriteMode::READ_WRITE, &pool);
  if (OB_FAIL(rc)) {
    return rc;
  }

  PageNum page = INVALID_PAGE_NUM;
  rc = pool->allocate_page(&page);
  if (OB_FAIL(rc)) {
    return rc;
  }

  {
    PageHandle handle;
    rc = pool->get_page_write(page, handle);
    if (OB_FAIL(rc)) {
      return rc;
    }
    memset(handle.value(), 0, BP_PAGE_SIZE);
    rc = handle.mark_dirty();
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  rc = pool->flush_all_buffers();
  if (OB_FAIL(rc)) {
    return rc;
  }

  return bpm.close();
}
```
