#!/usr/bin/env bash
# 生成「优化前 / 优化后」对比用的 SQL 脚本。
#
# 用法：
#   bash tests/gen_compare_sql.sh [行数] [输出文件]
#   bash tests/gen_compare_sql.sh 20000 /tmp/compare.sql
#
# 生成的数据是确定性的（没有随机数），所以两个版本跑出来的正确答案必然一致：
#   bench(id 主键, cat, val, name)   共 N 行
#     id   = i          唯一
#     cat  = i % 100    100 个不同值，每个值 N/100 行
#     val  = i % 5000   5000 个不同值，每个值 N/5000 行
#     name = 'n<i>'     没有索引
#   dim(k, g)           10 行：k = 0..9，g = k % 10
#
# 只依赖 bash/awk，Git Bash 和 WSL 里都能跑。

set -euo pipefail

ROWS=${1:-20000}
OUT=${2:-/tmp/compare.sql}

{
  echo "-- 由 tests/gen_compare_sql.sh 生成：$ROWS 行"
  echo "drop table if exists bench;"
  echo "drop table if exists dim;"
  echo "create table bench (id int primary key, cat int, val int, name char(16));"
  echo "create table dim (k int, g int);"

  # 每条 insert 塞 200 行，避免语句条数太多
  awk -v n="$ROWS" 'BEGIN {
    per = 200
    for (i = 0; i < n; i++) {
      if (i % per == 0) printf "%sinsert into bench values ", (i == 0 ? "" : ";\n")
      else printf ","
      printf "(%d,%d,%d,'\''n%d'\'')", i, i % 100, i % 5000, i
    }
    print ";"
    printf "insert into dim values "
    for (k = 0; k < 10; k++) printf "%s(%d,%d)", (k == 0 ? "" : ","), k, k % 10
    print ";"
  }'

  echo "create index bench_cat on bench (cat);"
  echo "create index bench_val on bench (val);"

  # B1 的复现夹具：char(1000) 的键很大，一页只放得下 8 个键，
  # 12 条相同的 'same' 必然跨多个叶子 —— 改前版本的范围扫描会漏掉左边叶子上的重复键。
  echo "drop table if exists dupcheck;"
  echo "create table dupcheck (id int, k char(1000));"
  echo "create index dup_k on dupcheck (k);"
  echo "insert into dupcheck values (1,'same'),(2,'same'),(3,'same'),(4,'same'),(5,'same'),(6,'same'),(7,'same'),(8,'same'),(9,'same'),(10,'same'),(11,'same'),(12,'same');"
  echo "insert into dupcheck values (13,'other'),(14,'other');"
} > "$OUT"

echo "生成完毕：$OUT（$ROWS 行 bench + 10 行 dim + 14 行 dupcheck）"
