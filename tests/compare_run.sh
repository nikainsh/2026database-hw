#!/usr/bin/env bash
# 一键对比「改前 / 改后」两个 minidb：同一份 SQL、同一份数据，比答案也比用时。
#
# 用法（必须在 WSL 里跑，Windows 上的计时精度只有 15.6ms，看不出差别）：
#   wsl -d Ubuntu-22.04
#   cd /mnt/e/dbhw && bash build.sh all && cd /mnt/e/database && bash build.sh all
#   cd /mnt/e/dbhw && bash tests/compare_run.sh [行数] [重复次数]
#
# 可用环境变量覆盖：
#   BASE_BIN  改前版本可执行文件，默认 /mnt/e/database/build/bin/minidb
#   OPT_BIN   改后版本可执行文件，默认 ./build/bin/database
#   WORK      工作目录，默认 /tmp/minidb_compare
#
# 输出两部分：
#   1) 答案对比：哪几条查询改前算错了
#   2) 用时对比：每条查询重复 N 次的中位数，以及加速倍数

set -uo pipefail

ROWS=${1:-20000}
REPEAT=${2:-5}
HERE=$(cd "$(dirname "$0")" && pwd)
BASE_BIN=${BASE_BIN:-/mnt/e/database/build/bin/minidb}
OPT_BIN=${OPT_BIN:-./build/bin/database}
WORK=${WORK:-/tmp/minidb_compare}

if [ "$(uname -s)" != "Linux" ]; then
  echo "请在 WSL 里运行：wsl -d Ubuntu-22.04  然后  cd /mnt/e/dbhw && bash tests/compare_run.sh"
  echo "（Windows 上的 minidb 计时被系统时钟量化成 15.6ms，测不出优化效果）"
  exit 1
fi

for bin in "$BASE_BIN" "$OPT_BIN"; do
  if [ ! -x "$bin" ]; then
    echo "找不到可执行文件：$bin"
    echo "先编译：cd /mnt/e/database && bash build.sh all ；cd /mnt/e/dbhw && bash build.sh all"
    exit 1
  fi
done

rm -rf "$WORK"
mkdir -p "$WORK"

echo "== 准备数据：bench $ROWS 行 + dim 10 行 =="
bash "$HERE/gen_compare_sql.sh" "$ROWS" "$WORK/data.sql" >/dev/null

# 名字表要和 compare_timing.sql 里的查询一一对应、顺序一致
NAMES=(
  "count(*) 全表 [P0]"
  "count(*) cat>=0 [P1a]"
  "count(*) cat<50 [P1a]"
  "count(*) id>2 and cat<4 [A1]"
  "join bench.id=dim.k [P1b]"
  "join bench.cat=dim.g [P1b]"
  "group by cat<3 [对照组]"
  "count(*) 重复键跨叶子 [B1]"
)
NQ=${#NAMES[@]}

# 期望答案（按 ROWS 算）：每个 cat 值有 ROWS/100 行
PER=$((ROWS / 100))
EXPECT=(
  "$ROWS"
  "$((PER * 100))"
  "$((PER * 50))"
  "$((4 * PER - 3))"
  "10"
  "$((PER * 10))"
  "0,$PER;1,$PER;2,$PER"
  "12"
)

run_one() {  # run_one <bin> <datadir> <标签>
  local bin=$1 data=$2 tag=$3
  echo "  载入数据到 $tag ..."
  "$bin" --data "$data" --db t -f "$WORK/data.sql" >"$WORK/$tag.load.log" 2>&1
  for i in $(seq "$REPEAT"); do cat "$HERE/compare_timing.sql"; done >"$WORK/$tag.rep.sql"
  "$bin" --data "$data" --db t -f "$WORK/$tag.rep.sql" >"$WORK/$tag.raw" 2>&1
  tr -d '\000\r' <"$WORK/$tag.raw" | awk -v nq="$NQ" '
    /^(minidb|database)> / { blk++; cells=""; nhdr=0; next }
    /^ *[0-9]+ rows/ { next }
    /\([0-9]+ us\)/ { t=$0; gsub(/[^0-9]/,"",t); print blk, cells, t; next }
    /\|/ {
      line=$0
      if (line ~ /^[-+ |]+$/) next
      if (nhdr == 0) { nhdr=1; next }
      gsub(/[ ]/,"",line); gsub(/\|/,",",line); sub(/^,/,"",line); sub(/,$/,"",line)
      cells = (cells=="" ? line : cells ";" line)
      next
    }' >"$WORK/$tag.parsed"
}

echo "== 两个版本各跑一遍（每条查询重复 $REPEAT 次）=="
run_one "$BASE_BIN" "$WORK/base" before
run_one "$OPT_BIN"  "$WORK/opt"  after

summarize() {  # summarize <parsed文件> -> 每条查询一行：答案 中位数
  awk -v nq="$NQ" '
    { idx = (($1 - 1) % nq) + 1; ans[idx] = $2; times[idx] = times[idx] " " $3 }
    END {
      for (i = 1; i <= nq; i++) {
        n = split(times[i], a, " ")
        for (x = 1; x <= n; x++) for (y = x + 1; y <= n; y++) if (a[y] < a[x]) { t = a[x]; a[x] = a[y]; a[y] = t }
        mid = int((n + 1) / 2)
        printf "%s\t%d\n", ans[i], a[mid]
      }
    }' "$1"
}

summarize "$WORK/before.parsed" >"$WORK/before.sum"
summarize "$WORK/after.parsed"  >"$WORK/after.sum"

echo
echo "== 1) 答案对比（正确答案按数据算好，见下面「期望」列）=="
printf '%-30s %-14s %-14s %-14s %s\n' "查询" "期望" "改前" "改后" "结论"
for i in $(seq "$NQ"); do
  b_ans=$(sed -n "${i}p" "$WORK/before.sum" | cut -f1)
  a_ans=$(sed -n "${i}p" "$WORK/after.sum" | cut -f1)
  exp=${EXPECT[$((i - 1))]}
  verdict="两边都对"
  [ "$b_ans" != "$exp" ] && [ "$a_ans" = "$exp" ] && verdict="改前算错，改后修正"
  [ "$b_ans" != "$exp" ] && [ "$a_ans" != "$exp" ] && verdict="两边都不对（检查脚本）"
  [ "$b_ans" = "$exp" ] && [ "$a_ans" != "$exp" ] && verdict="改后算错（回归！）"
  printf '%-30s %-14s %-14s %-14s %s\n' "${NAMES[$((i - 1))]}" "$exp" "$b_ans" "$a_ans" "$verdict"
done

echo
echo "== 2) 用时对比（微秒，$REPEAT 次中位数）=="
printf '%-30s %10s %10s %10s\n' "查询" "改前" "改后" "加速"
tb=0; ta=0; starred=0
for i in $(seq "$NQ"); do
  b=$(sed -n "${i}p" "$WORK/before.sum" | cut -f2)
  a=$(sed -n "${i}p" "$WORK/after.sum" | cut -f2)
  b_ans=$(sed -n "${i}p" "$WORK/before.sum" | cut -f1)
  tb=$((tb + b)); ta=$((ta + a))
  speed=$(awk -v b="$b" -v a="$a" 'BEGIN { if (a <= 0) a = 1; printf "%.1fx", b / a }')
  if [ "$b_ans" != "${EXPECT[$((i - 1))]}" ]; then
    # 改前答案是错的，它的用时只能当参考：错得越快越没用
    speed="$speed *"; starred=1
  fi
  printf '%-30s %10d %10d %10s\n' "${NAMES[$((i - 1))]}" "$b" "$a" "$speed"
done
speed=$(awk -v b="$tb" -v a="$ta" 'BEGIN { if (a <= 0) a = 1; printf "%.1fx", b / a }')
printf '%-30s %10d %10d %10s\n' "合计" "$tb" "$ta" "$speed"
if [ "$starred" = "1" ]; then
  echo
  echo "* 改前这条的答案是错的（见上面的答案对比），它的用时只能当参考：算错还慢，才是真正的问题。"
fi

echo
echo "原始输出留在 $WORK（before.raw / after.raw 是完整终端输出，可自己核对）"
