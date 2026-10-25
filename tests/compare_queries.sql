-- 手工测试用的查询清单：先用 tests/gen_compare_sql.sh 生成数据并导入，再跑这个文件。
-- 「改前」= E:\database，「改后」= E:\dbhw。两边跑的是同一份文件，命令和正确答案完全一样，
-- 只有用时和个别「改前算错的答案」不同。每条命令 CLI 都会在结果后面打印 (N us)。
--
-- 下面的期望值按 20000 行数据算好；换行数要重算（cat = i%100，所以每个 cat 值有 N/100 行）。
-- 想看两版并排的表格，直接跑 tests/compare_run.sh，不用手工比。

-- ===== 0. 基本操作 =====
show tables;
desc bench;

-- ===== 1. 主键点查（两版都走索引，用来确认基线一致）=====
explain select count(*) from bench where id = 5000;
select count(*) from bench where id = 5000;              -- 期望 1

-- ===== 2. 非唯一索引等值查询 =====
explain select count(*) from bench where cat = 7;
select count(*) from bench where cat = 7;                -- 期望 200（int 键一页能放很多，两版都对）

-- ===== 3. 覆盖全表的范围条件（对应优化 P1a）=====
explain select count(*) from bench where cat >= 0;
select count(*) from bench where cat >= 0;               -- 期望 20000；改前走索引回表，慢 3 倍左右

-- ===== 4. 两个都建了索引的列各带一个范围条件（对应修复 A1）=====
explain select count(*) from bench where id > 2 and cat < 4;
select count(*) from bench where id > 2 and cat < 4;     -- 期望 797；改前返回 1（把 cat<4 当成了 id 的上界）

-- ===== 5. 无条件 count(*)（对应优化 P0）=====
explain select count(*) from bench;
select count(*) from bench;                              -- 期望 20000；改前要扫全表，改后读表头
select count(*) from bench where cat < 50;               -- 期望 10000；带条件时不能走捷径

-- ===== 6. 聚合与分组（对照组：两版应该差不多）=====
select count(*), sum(cat), avg(val), min(id), max(id) from bench;   -- 期望 20000 / 990000 / 2499.5 / 0 / 19999
select cat, count(*) from bench where cat < 3 group by cat;         -- 期望 3 行，每行 200

-- ===== 7. 两表连接（对应优化 P1b）=====
explain select count(*) from bench, dim where bench.id = dim.k;
select count(*) from bench, dim where bench.id = dim.k;   -- 期望 10；改前是 20000×10 次配对
explain select count(*) from bench, dim where bench.cat = dim.g;
select count(*) from bench, dim where bench.cat = dim.g;  -- 期望 2000

-- ===== 8. 重复键跨多个叶子时的范围扫描（对应修复 B1）=====
explain select count(*) from dupcheck where k = 'same';
select count(*) from dupcheck where k = 'same';           -- 期望 12；改前只返回 7（漏掉左边叶子上的重复键）
select count(*) from dupcheck where k = 'other';          -- 期望 2

-- ===== 9. 改数据之后计数仍然准确（P0 的捷径不能算错）=====
update bench set val = 1 where id = 3;
select id, val from bench where id = 3;                   -- 期望 3 / 1
delete from bench where id = 19999;
-- 期望 19999：删过行之后表头计数也跟着减了，捷径不会算错
select count(*) from bench;
