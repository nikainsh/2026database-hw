# database 图形界面

使用 Python 3 的标准库 Tkinter，无须 pip 安装。先在仓库根目录编译数据库，再启动界面：

```powershell
& "请填写 Git Bash 实际安装目录/bin/bash.exe" build.sh
python gui/database_gui.py
```

第一行 Git Bash 路径按自己的安装位置调整。Linux/macOS 可以执行 `bash build.sh`、`python3 gui/database_gui.py`，需要系统提供 Tkinter。

## 使用示例

1. 保持默认数据库 `gui_demo` 和数据目录 `build/gui_data`。
2. 加载“建表及数据”，检查 SQL，点击“运行 SQL”。这会重建 `gui_students`、`gui_scores` 两张演示表。
3. 点击“连接 / 刷新表”，双击表名可以查看字段结构。
4. 加载“联合键 Hash Join”，点击“查看 EXPLAIN”，展示物理计划中的 `hash_join` 和 `key_count=2`。
5. 点击“运行 SQL”，展示正确的三条匹配结果；切换到“执行统计”，解释 build/probe 行数、候选比较次数和估算内存。
6. 加载“连接与分组”，运行后展示 Alice 的平均成绩 92.5、Bob 的平均成绩 85。
7. “导出当前结果 CSV”保存当前显示的数据，便于查看和核对结果。

## 使用说明与边界

- Ctrl+Enter 或运行按钮执行选中 SQL；没有选区则执行全文，多条语句以分号分隔。
- 结果页下拉框切换每条语句的结果，错误、历史和 DDL 提示在消息页查看。建表后手动刷新对象列表。
- EXPLAIN 按钮仅用于单条 SELECT。它只生成计划；应运行原查询后查看真正的执行算子统计。
- SQL 报错停止本次脚本，之前成功的语句不会自动回滚。本界面不增加事务支持。
- 每条查询执行完成并统计全部行数，只保留前 1000 行用于显示和 CSV 导出；不是 SQL LIMIT，也不用于性能基准计时。
- SQL 在后台线程中调用真实 database 子进程，默认 120 秒超时。每次请求重新打开并关闭数据库；不支持并发访问同一个数据目录中的同一个库，运行界面时请避免另外打开同库的 CLI 或其他窗口。
- 默认使用独立演示库。若连接已有数据库，数据目录选到包含数据库目录的父目录，再填写对应库名；不存在则自动创建。
- SQL/结果通过 UTF-8 JSON 传输，单元格使用字符串保留 BIGINT 精度，包含分隔符、换行等字符也不会被 ASCII 表格截断。数据库名限定英文字母、数字、下划线，且不以数字开头。
- GUI 本身不改变引擎支持的 SQL 范围。Windows 下通过子进程工作目录与相对数据路径避开 MinGW 中文绝对路径的文件访问问题。

## 测试

```powershell
python gui/test_gui.py
```

测试使用 `build` 下自动清理的临时库，覆盖真实 SQL 执行、联合键计划与统计、中文/特殊字符、空结果、SQL 错误中止、NUMERIC_OVERFLOW、结果截断计数、EXPLAIN 单语句保护及 Tk 结果切换。

## 文件分工

- `gui/database_gui.py`：桌面布局、后台执行、结果与计划展示、CSV 导出。
- `gui/backend.py`：启动真实数据库程序，处理 UTF-8 JSON，提供演示 SQL。
- `gui/test_gui.py`：真实后端及 Tk 界面组件的集成测试。
- `src/shell/json_output.h`：实现 ResultWriter，序列化结果、错误和统计，限制显示缓存。
- `src/shell/main.cpp`：增加 `--json` / `--json-single` 输入输出模式，原 CLI 入口保留。

结构为 `Tkinter → 子进程 stdin → QueryEngine → ResultWriter(JSON) → 结果页`。本次改动属于用户界面层，查询处理、执行算子、存储和缓冲算法沿用现有实现。
