# database：关系型数据库管理系统

database 是使用 C++17 实现的教学型关系数据库项目，覆盖 SQL 解析、语义检查、查询优化、物理计划执行、记录存储、B+ 树索引和磁盘缓冲管理等环节。项目提供命令行和 Python Tkinter 图形界面，可用于运行 SQL、查看执行计划、观察算子统计及验证数据持久化。

系统借鉴 MiniOB 的模块划分，采用简化的关系模型与火山式执行模型。GUI 与命令行共用同一套 C++ 查询与存储实现。

## 目录

- [主要功能](#主要功能)
- [系统结构与模块](#系统结构与模块)
- [环境要求](#环境要求)
- [编译与启动](#编译与启动)
- [SQL 使用示例](#sql-使用示例)
- [图形界面](#图形界面)
- [执行计划与统计](#执行计划与统计)
- [测试与性能实验](#测试与性能实验)
- [数据文件与使用边界](#数据文件与使用边界)
- [常见问题](#常见问题)
- [相关文档](#相关文档)

## 主要功能

| 类别 | 已实现功能 |
| --- | --- |
| 表与索引管理 | 创建表、删除表、查看表与字段结构、创建普通及唯一索引、主键索引 |
| 数据读写 | INSERT、SELECT、UPDATE、DELETE；多行插入与条件过滤 |
| 关系运算 | 投影、多表内连接、分组、COUNT / SUM / AVG / MIN / MAX 聚合 |
| 查询优化 | 谓词下推、常量比较化简、按基数调整连接顺序、索引访问路径选择 |
| 连接执行 | 嵌套循环连接、索引嵌套循环连接、单键及多列联合键 Hash Join |
| 存储与缓冲 | 堆文件、槽式页、RID 定位、B+ 树、共享缓冲帧、LRU-K、异步预取、后台刷脏、页级 WAL 与校验 |
| 可观测性 | EXPLAIN 逻辑与物理计划、算子级执行统计、连接性能实验 |
| 用户交互 | CLI 交互及脚本执行、GUI SQL 编辑与结果表格、计划及统计查看、CSV 导出 |

## 系统结构与模块

```text
命令行 CLI / Tkinter GUI
          │ SQL
          ▼
查询处理器：词法与语法分析 → 语义检查 → 逻辑计划 → 优化 → 物理计划
          │
          ▼
执行引擎：open / next / close → 扫描、过滤、连接、投影、聚合、写入
          │
          ▼
存储管理器：数据库目录、表、堆文件、记录、B+ 树索引
          │
          ▼
缓冲管理器：共享帧池、页缓存与淘汰、预取、刷脏、页级 WAL
          │
          ▼
磁盘文件
```

### 1. 存储管理器

位于 `src/storage/db`、`table`、`record`、`index`。

- `Db` 管理数据库目录、系统目录文件以及表和索引的元数据。
- `Table` 统一记录与索引的读写，维护插入、删除和更新时的索引一致性。
- 堆文件通过槽式页保存记录，以 RID（页号、槽号）定位数据；支持扫描、空间复用和记录计数。
- B+ 树支持等值查询、范围扫描、重复键、唯一性检查、节点分裂、删除借位与合并。

### 2. 缓冲管理器

位于 `src/storage/buffer`。

- 以 8 KB 页为单位读写文件，通过全局 `FrameManager` 共享缓冲帧。
- 使用 LRU-K 选择淘汰页，结合 pin 计数与 RAII 页句柄控制页的使用周期。
- 提供页读写锁、异步预取、后台刷脏和缓冲统计。
- 提供页校验、页级 redo WAL、重放和检查点，用于缓冲层的故障恢复。

### 3. 查询处理器

位于 `src/sql/parser`、`stmt`、`optimizer` 及计划生成相关代码。

- 将 SQL 转换为语法结构，检查表名、字段名、别名及类型。
- 生成逻辑计划，执行谓词下推、比较式化简和连接顺序调整。
- 选择全表扫描或索引扫描，并根据连接条件、索引和估算信息生成物理算子树。
- `EXPLAIN` 输出优化后的逻辑计划与物理计划。

### 4. 执行引擎

位于 `src/sql/operator`、`plan`、`expr`，由 `src/sql/query_engine.cpp` 驱动。

- 算子通过 `open / next / close` 接口逐行传递元组，统一处理执行结束、错误传播和资源释放。
- 实现表扫描、索引扫描、过滤、投影、连接、标量聚合、哈希分组及 DML 算子。
- Hash Join 支持单键和多列联合键等值连接，保留重复匹配结果，并限制构建端的估算内存占用。
- 聚合处理包含整数 SUM 溢出检查、AVG 精度处理和 FLOAT 结果范围检查。
- 可采集每个算子的输入输出行数、调用次数、耗时、候选比较次数和 Hash Join 构建信息。

### 5. 用户界面

- `src/shell`：命令行交互、SQL 文件执行、JSON 输出适配。
- `gui`：Python Tkinter 桌面界面，通过子进程调用真实数据库程序；使用 UTF-8 JSON 接收结果，不解析命令行表格文本。
- `ResultWriter`：执行引擎与结果展示之间的接口，供 CLI 和 JSON 输出分别实现。

### 仓库目录

```text
.
├── src/
│   ├── common/              类型、值、错误码、日志及文件工具
│   ├── shell/               CLI 与 JSON 输出
│   ├── sql/
│   │   ├── parser/          词法及语法分析
│   │   ├── stmt/            语义检查与语句对象
│   │   ├── optimizer/       查询优化与计划生成
│   │   ├── plan/            逻辑及物理算子基础设施
│   │   ├── operator/        扫描、连接、聚合、DML 等算子
│   │   └── expr/            表达式、元组与聚合状态
│   └── storage/
│       ├── db/              数据库与系统目录
│       ├── table/           表与表元数据
│       ├── record/          堆文件、槽式页与记录扫描
│       ├── index/           B+ 树索引
│       └── buffer/          缓冲帧、替换、预取、WAL
├── gui/                     图形界面及其集成测试
├── tests/                   C++ 测试、SQL 样例与验证脚本
├── bench/                   缓冲池性能实验
├── docs/                    模块接口和设计文档
├── build.sh                 Bash 构建入口
├── Makefile                 Make 构建入口
└── CMakeLists.txt           CMake 构建入口
```

## 环境要求

- 支持 C++17 的 GCC / G++ 工具链。
- 使用 `build.sh` 时需要 Bash 及常用命令行工具；Windows 可使用 Git Bash，并将 G++ 加入 PATH。
- 使用 GUI 时需要 Python 3 和 Tkinter；GUI 不依赖第三方 pip 包。可执行 `python -m tkinter` 检查 Tkinter 是否可用。
- Make 与 CMake 为可选构建方式。当前构建参数面向 GCC / MinGW 风格工具链。

下列命令均在仓库根目录执行。出现“请填写实际目录”或“请填写 Git Bash 实际安装目录”时，请替换为本机路径。

## 编译与启动

### Windows：PowerShell + Git Bash

```powershell
Set-Location "请填写实际目录/2026database-hw"

# Git Bash 已加入 PATH 时
bash build.sh

# 若 bash 无法找到，使用实际安装路径调用
& "请填写 Git Bash 实际安装目录/bin/bash.exe" build.sh

# 上面两种编译命令选择一种即可
.\build\bin\database.exe --db sample --data ./data
```

编译产物为 `build/bin/database.exe`。启动时自动打开指定库；库不存在时会创建。

### Linux / macOS：Bash

```bash
cd "请填写实际目录/2026database-hw"
bash build.sh
./build/bin/database --db sample --data ./data
```

编译产物为 `build/bin/database`。

### 命令行参数

| 参数 | 作用 |
| --- | --- |
| `--db <name>` | 数据库名，默认 `testdb` |
| `--data <dir>` | 数据根目录，默认 `./data`；实际库目录为 `<dir>/<name>` |
| `-e <sql>` | 执行一条 SQL 后退出 |
| `-f <file>` | 执行 SQL 文件后退出 |
| `--profile` | 输出算子级执行统计 |
| `--quiet` | 隐藏结果行输出 |
| `--verbose` | 输出调试日志 |
| `--json` | 从标准输入读取 SQL 脚本，输出 JSON，供 GUI 等程序调用 |
| `--json-single` | JSON 模式，仅接受一条语句 |
| `-h` / `--help` | 查看启动参数 |

Windows 示例：

```powershell
.\build\bin\database.exe --db sample --data ./data -e "show tables;"
.\build\bin\database.exe --db sample --data ./data --profile
.\build\bin\database.exe --db example --data ./build/example_data -f tests/demo.sql
```

Linux / macOS 将可执行程序路径替换为 `./build/bin/database` 即可。`tests/demo.sql` 包含重建示例表的语句，请在独立示例库中运行。

CLI 中 SQL 以分号结束，支持多行输入。输入 `help` 查看支持的语法，输入 `exit` 或 `quit` 退出。

### 其他构建方式

在具备 Make 与 Bash 工具的环境中：

```bash
make
```

使用 CMake 与 GCC 工具链：

```bash
cmake -S . -B build_cmake
cmake --build build_cmake
ctest --test-dir build_cmake --output-on-failure
```

CMake 的可执行程序位于其构建目录，具体位置取决于生成器。GUI 默认查找 `build/bin/database[.exe]`，因此 GUI 用户建议使用 `build.sh` 或 Make 构建。

## SQL 使用示例

在新建的 `sample` 数据库中依次执行：

```sql
create table student (
  id int,
  name char(16),
  age int,
  score float,
  primary key(id)
);

insert into student values
  (1, 'alice', 20, 88.5),
  (2, 'bob', 22, 92.0),
  (3, 'carol', 20, 76.0);

show tables;
desc student;

select * from student;
select id, name from student where age >= 20 and score > 80;
select age, count(*), avg(score) from student group by age;

create index idx_student_age on student(age);
explain select * from student where id = 2;

update student set score = 95.5 where id = 1;
delete from student where age < 20;
```

多表连接使用逗号分隔表，在 WHERE 中指定连接条件：

```sql
create table course_score (student_id int, term int, score int);
create table enrollment (student_id int, term int, course char(16));

insert into course_score values (1, 1, 90), (1, 2, 95), (2, 1, 85);
insert into enrollment values (1, 1, 'database'), (1, 2, 'systems'), (2, 1, 'database');

select e.student_id, e.course, g.score
from enrollment e, course_score g
where e.student_id = g.student_id and e.term = g.term;

explain select e.student_id, e.course, g.score
from enrollment e, course_score g
where e.student_id = g.student_id and e.term = g.term;
```

该连接包含两对等值键，可以通过物理计划检查是否选择 `hash_join` 以及 `key_count=2`。联合连接键与数据库联合索引是不同的概念。

常用列类型包括 `int`、`bigint`、`float`、`char(n)`。字符串使用单引号，内部单引号可写为两个单引号。支持 `--` 行注释和 `/* ... */` 块注释。

## 图形界面

先完成 C++ 程序编译，再运行：

```powershell
python gui/database_gui.py
```

Linux / macOS 通常使用 `python3 gui/database_gui.py`。若 Python 未加入 PATH：

```powershell
& "请填写 Python 实际安装目录/python.exe" gui/database_gui.py
```

### 界面功能

| 区域 | 使用方法 |
| --- | --- |
| 数据库与数据目录 | 填写库名和数据根目录，点击“连接 / 刷新表”；不存在的库会自动创建 |
| 数据库对象 | 显示表名，双击表名查看字段结构；建表后可手动刷新 |
| SQL 编辑器 | 支持多行 SQL；运行按钮或 Ctrl+Enter 执行选区，无选区时执行全文 |
| 查询结果 | 表格显示结果；下拉框切换脚本中各条语句的结果 |
| 执行计划 | 点击“查看 EXPLAIN”查看单条 SELECT 的逻辑及物理计划 |
| 执行统计 | 勾选采集统计并运行查询后，查看真实算子的运行数据 |
| 消息 / 历史 | 查看执行状态、错误、建表等提示及 SQL 历史 |
| CSV 导出 | 导出当前显示的结果，使用 UTF-8 BOM 编码 |

### 首次使用

1. 保持默认库名 `gui_demo` 和默认数据目录 `build/gui_data`。
2. 点击“建表及数据”加载 SQL，检查后点击“运行 SQL”。此脚本会重建 `gui_students`、`gui_scores` 两张示例表。
3. 点击“连接 / 刷新表”查看表列表。
4. 加载“联合键 Hash Join”，查看 EXPLAIN，再运行原查询；正常返回三条匹配结果。
5. 加载“连接与分组”运行，可得到 Alice 平均分 92.5、Bob 平均分 85。

GUI 每条查询执行完成并统计全部结果行数，最多缓存、显示和导出前 1000 行。每次请求会打开并关闭数据库，使用后台线程避免阻塞界面，默认请求超时为 120 秒。GUI 耗时不应直接作为数据库算子性能基准。

GUI 数据库名使用英文字母、数字和下划线，且不以数字开头。连接已有数据库时，数据目录应选择库目录的父目录，例如库文件在 `data/sample/` 下，应选择 `data` 并填写库名 `sample`。

更多说明见 [GUI 使用文档](gui/README.md)。

## 执行计划与统计

`EXPLAIN SELECT ...` 只生成计划。逻辑计划描述查询的关系运算，物理计划显示实际使用的扫描、过滤、连接和聚合算子；它不执行原 SELECT。

启动 CLI 时添加 `--profile`，或在 GUI 勾选“采集执行统计”，然后执行查询，可查看：

- `in_rows` / `out_rows`：算子输入与输出行数；输入行数按子算子的累计输出计算。
- `opens` / `nexts` / `closes` / `errors`：生命周期调用及错误计数。
- `candidates`：连接候选元组对的比较次数。
- `build_rows` / `probe_rows`：Hash Join 构建与探测行数。
- `peak_build_bytes_est`：Hash Join 构建存储的估算峰值，不是进程实际内存占用。
- `open_us` / `next_us` / `close_us` / `inclusive_us`：执行耗时，父算子时间包含子算子调用，不宜将整棵树的耗时直接相加。

嵌套循环连接可能多次打开并扫描内表，因此累计行数可能大于表的实际记录数。索引条件、类型和估算信息都会影响最终选择的执行计划。

## 测试与性能实验

以下 Bash 命令在 Windows Git Bash 或 Linux / macOS 终端中执行；PowerShell 可用 `bash`，或通过前文的 Git Bash 实际路径调用。

```bash
# 编译并运行全部 C++ 测试；遇到失败会停止
bash build.sh test

# 现有 SQL 端到端验证
bash tests/run_e2e.sh

# 执行引擎专项测试，自动构建所需程序
bash tests/run_execution_tests.sh

# Hash Join 正确性检查及性能实验
bash tests/run_execution_tests.sh --bench --rows 1000 --repeat 7 --csv build/hash_join_benchmark.csv

# 缓冲池性能实验
bash bench/run_bench.sh
```

GUI 集成测试：

```powershell
python gui/test_gui.py
```

- C++ 测试覆盖记录管理、B+ 树、索引统计、缓冲并发、预取、WAL、恢复与执行算子等模块。
- GUI / CLI 集成测试覆盖真实 SQL 调用、中文及特殊字符、脚本注释和转义、脚本退出码、计划与统计、空结果、错误中止、数值溢出、结果显示上限和旧结果清理。
- Hash Join 实验比较嵌套循环、单键 Hash Join 与联合键 Hash Join。三种计划采用相同的双键等值语义，计时前先校验完整结果。
- 实验输出汇总 CSV 与 `.samples.csv` 原始样本，包含不同键分布、重复键、无匹配、空输入与宽记录等场景。该实验在内存中构造算子，不包含 SQL 规划、磁盘 I/O 与终端输出。

测试脚本会创建或清理专用测试数据目录；`tests/run_e2e.sh` 使用 `e2e_data/`。不要在这些目录中保存需要保留的数据。崩溃恢复测试直接启动子进程：Windows 使用 Unicode 路径接口，POSIX 使用 fork / exec；每个场景先验证故障注入退出码，再校验恢复后的页面及 WAL 状态。

## 数据文件与使用边界

### 数据位置

CLI 默认保存到 `data/testdb/`，GUI 默认保存到 `build/gui_data/gui_demo/`。数据库目录包含 `catalog.bin`、表的 `.heap` 文件、索引的 `.index` 文件，以及对应的 WAL 文件。

关闭后使用相同 `--data` 和 `--db` 重新打开即可继续访问数据。更换工作目录后，相对路径指向的位置也会变化。

`bash build.sh clean` 会删除构建产物以及脚本列出的数据目录，其中包括 `data/`；`bash build.sh rebuild` 会删除整个 `build/`。默认 GUI 数据位于 `build/` 下，清理或重建前请先关闭数据库并备份需要保留的整个库目录，或将数据目录设置在这些清理范围之外。

### 当前范围

- 项目实现的是 SQL 子集，不保证与 MySQL、PostgreSQL 等数据库完全兼容；当前不提供子查询、外连接、ORDER BY、LIMIT、HAVING 等完整 SQL 能力。
- 多表查询使用 `FROM a, b WHERE ...` 写法。Hash Join 用于支持类型的内等值连接，键分量支持同类型的 INT、BIGINT、CHARS；不支持 FLOAT 或 NULL 哈希键，不执行隐式键类型转换，也不提供磁盘溢写。
- Hash Join 默认构建端估算内存预算为 8 MiB。物理计划选择会考虑适用条件；运行中超出预算时返回错误，不会自动将正在执行的计划改成磁盘连接。
- 页级 WAL 提供缓冲层恢复能力，不等于完整的 SQL 事务系统；未实现 SQL 级 BEGIN / COMMIT / ROLLBACK、MVCC 或跨多页操作的完整事务原子性。
- 脚本遇错后停止后续语句，之前成功的语句不会自动回滚；检查结果时应同时查看错误码和消息。
- 缓冲层具备进程内同步机制，但应用层不支持多个进程并发打开同一数据库进行读写。使用 GUI 时应避免同时用另一个 GUI 或 CLI 打开同一个库。

## 常见问题

**找不到 G++ 或 Bash。** 检查工具是否安装并加入 PATH。Windows 下可用 Git Bash 的实际安装路径启动脚本；需要时在 Bash 中通过 `CXX` 指定 G++。

**启动 GUI 提示找不到程序或返回的 JSON 无效。** 先在当前仓库运行 `bash build.sh`，确认 `build/bin/database[.exe]` 已更新，再启动 GUI。只拉取源码不会自动更新本地可执行文件。

**链接时提示 Permission denied。** 关闭正在运行的数据库程序，并等待 GUI 查询完成，再重新编译。

**更换工具链后链接失败。** 使用同一套编译器重新构建。构建脚本按编译器标识区分对象目录；清理构建目录前先确认其中的数据已经备份。

**提示 db not found 后自动创建新库。** 首次使用新库名时属于正常行为。如果预期打开已有库，请核对数据根目录、库名和当前工作目录。

**Windows 下中文路径访问异常。** CLI 的数据目录和 SQL 文件参数优先使用相对路径。GUI 已通过工作目录与相对数据路径调用后端；Python 自身需要能正常启动仓库中的程序。

**Tkinter 无法导入。** 使用带有 Tkinter 的 Python 发行版，或按系统方式安装 Tk 支持。仅安装第三方 GUI 包不能替代标准库 Tkinter。

## 相关文档

- [GUI 使用说明](gui/README.md)
- [模块接口](docs/api.md)
- [存储管理器设计](docs/storage-manager.md)
- [全局缓冲帧管理设计](docs/buffer-v3.md)
- [SQL 综合示例](tests/demo.sql)
- [联合键 Hash Join 示例](tests/execution_v6_composite_hash_join.sql)

模块文档用于补充设计细节，具体行为以当前源码、执行计划及测试结果为准。
