#!/usr/bin/env bash
# minidb 端到端自测：跑示例脚本、验证持久化、验证优化器选择了索引、验证错误路径。
#
#   bash tests/run_e2e.sh
set -u
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/bin/minidb}
[ -x "${BIN}.exe" ] && BIN="${BIN}.exe"
if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件 $BIN，请先构建：bash build.sh"
  exit 1
fi
DATA=./e2e_data
PASS=0
FAIL=0

check() {  # check <描述> <期望片段> <实际输出>
  local what="$1" expect="$2" actual="$3"
  if echo "$actual" | grep -qF -- "$expect"; then
    echo "  PASS  $what"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $what  (未在输出中找到: $expect)"
    echo "  ---- 实际输出 ----"
    echo "$actual" | sed 's/^/  /'
    echo "  ------------------"
    FAIL=$((FAIL + 1))
  fi
}

run() { $BIN --data "$DATA" --db e2e "$@" 2>&1; }

expect_error() {  # expect_error <描述> <sql>
  local what="$1" sql="$2"
  if run -e "$sql" | grep -q "ERROR \["; then
    echo "  PASS  $what"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $what  (期望报错但没有): $sql"
    FAIL=$((FAIL + 1))
  fi
}

expect_ok() {  # expect_ok <描述> <sql>
  local what="$1" sql="$2"
  if run -e "$sql" | grep -q "ERROR \["; then
    echo "  FAIL  $what  (意外报错): $sql"
    run -e "$sql" | sed 's/^/    /'
    FAIL=$((FAIL + 1))
  else
    echo "  PASS  $what"
    PASS=$((PASS + 1))
  fi
}

## 断言某个 count(*) 查询返回的确切数值（比子串 grep 强，用来守正确性回归）
expect_count() {  # expect_count <描述> <sql> <期望值>
  local what="$1" sql="$2" want="$3"
  local got
  got=$(run -e "$sql" | awk '/^[[:space:]]*[0-9]+[[:space:]]*\|/ {gsub(/[^0-9]/, "", $0); print; exit}')
  if [ "$got" = "$want" ]; then
    echo "  PASS  $what (= $want)"
    PASS=$((PASS + 1))
  else
    echo "  FAIL  $what  期望 $want 实际 '${got:-无输出}'"
    run -e "$sql" | sed 's/^/    /'
    FAIL=$((FAIL + 1))
  fi
}

rm -rf "$DATA"
echo "== 1) 跑完整示例脚本 tests/demo.sql =="
demo_output=$(timeout 120 $BIN --data "$DATA" --db e2e -f tests/demo.sql 2>&1)
demo_exit=$?

check "脚本退出码为 0" "exit=0" "exit=$demo_exit"
check "show tables 列出两张表" "teacher" "$demo_output"
check "desc 显示主键唯一索引" "student_id_pk(unique)" "$demo_output"
check "全表扫描结果" "alice" "$demo_output"
check "条件过滤结果" "carol" "$demo_output"
check "两表连接" "| dave" "$demo_output"
check "create index 成功" "created index idx_age" "$demo_output"
check "explain 走主键索引" "index_scan(student.student_id_pk)" "$demo_output"
check "explain 里 age 也走索引" "index_scan(student.idx_age)" "$demo_output"
check "explain 显示无索引时的全表扫描" "table_scan(student)" "$(run -e 'explain select * from student where score > 90')"
check "聚合 count/sum/avg" "3" "$demo_output"
check "group by 输出" "age" "$demo_output"
check "update 生效" "95.5" "$demo_output"
check "delete 后计数" "2 rows" "$demo_output"

echo "== 2) 重开数据库验证持久化（缓冲管理器落盘 + catalog） =="
persisted=$(run -e 'select id, name, score from student')
check "重启后仍在" "bob" "$persisted"
check "重启后 alice 的分数已更新" "95.5" "$persisted"
count=$(run -e 'select count(*) from student')
if echo "$count" | grep -qE '^[[:space:]]*2[[:space:]]*\|'; then
  echo "  PASS  重启后行数为 2"
  PASS=$((PASS + 1))
else
  echo "  FAIL  重启后行数为 2"
  echo "$count" | sed 's/^/    /'
  FAIL=$((FAIL + 1))
fi
reopened=$(run -e 'show tables')
check "重启后表还在" "student" "$reopened"

echo "== 2b) 索引选择/扫描的正确性回归（A1、B1）=="
expect_count "非唯一索引跨叶子重复键全部返回" "select count(*) from dupcheck where k = 'same'" 12
expect_count "两个索引列各带范围条件时键范围不跨列串用" "select count(*) from twocol where id > 2 and cat < 4" 5

echo "== 2c) count(*) 走表头计数的正确性回归（P0）=="
expect_count "无条件 count(*) = 表头记录数" "select count(*) from dupcheck" 14
expect_count "建过索引后 count(*) 仍准确" "select count(*) from twocol" 8
expect_count "带条件时不能短路成全表行数" "select count(*) from twocol where id > 2" 5
expect_count "删过行之后 count(*) 仍准确" "select count(*) from student" 2


echo "== 2d) 连接条件与索引嵌套循环连接的正确性回归（P1b）=="
expect_count "无索引时连接行数" "select count(*) from join_small, join_big where join_small.k = join_big.k" 7
expect_count "无索引时连接命中的 id 之和" "select sum(join_big.id) from join_small, join_big where join_small.k = join_big.k" 29
expect_count "连接列建索引后行数不变" "select count(*) from join_small, join_big where join_small.k = join_big.k" 7
expect_count "连接列建索引后 id 之和不变" "select sum(join_big.id) from join_small, join_big where join_small.k = join_big.k" 29
expect_count "FROM 顺序反过来仍是同一批行" "select count(*) from join_big, join_small where join_big.k = join_small.k" 7
expect_count "连接条件与单表条件同时成立" "select count(*) from join_small, join_big where join_small.k = join_big.k and join_big.id > 2" 5
expect_count "没有连接条件时是笛卡尔积" "select count(*) from join_small, join_big" 32
check "explain 里连接走索引查找" "join_lookup" "$(run -e 'explain select count(*) from join_small, join_big where join_small.k = join_big.k')"
check "explain 把连接条件挂在 join 上" "on=join_small.k = join_big.k" "$(run -e 'explain select count(*) from join_small, join_big where join_small.k = join_big.k')"

echo "== 3) 索引内容与表内容保持一致 =="
after_delete=$(run -e 'select name from student where age = 20')
check "按索引列删除后仍能查到剩余行" "alice" "$after_delete"
expect_ok "按唯一索引列查不存在的值返回空" "select * from student where id = 999"

echo "== 4) 错误路径 =="
expect_error "表不存在"              "select * from no_such_table"
expect_error "列不存在"              "select nope from student"
expect_error "聚合与裸列混用"         "select id, count(*) from student"
expect_error "唯一索引冲突"          "insert into student values (1, 'dup', 20, 1.0)"
expect_error "语法错误"              "select * from student where"
expect_error "字符串超长"            "insert into student values (77, 'this_name_is_way_too_long_for_char16', 30, 1.0)"
expect_error "类型不匹配的等值比较"    "select * from student where id = 'abc'"
expect_error "group by 里没有的裸列"  "select name, count(*) from student group by age"
expect_error "drop 不存在的表"        "drop table ghost"
expect_error "重复建索引名"           "create index idx_age on student (score)"

echo "== 5) drop table 后数据文件应被清理 =="
run -e 'drop table teacher' >/dev/null
remaining=$(run -e 'show tables')
if echo "$remaining" | grep -q "teacher"; then
  echo "  FAIL  drop table 之后 teacher 仍然存在"
  FAIL=$((FAIL + 1))
else
  echo "  PASS  drop table 清理成功"
  PASS=$((PASS + 1))
fi
leftover=$(find "$DATA" -type f -name 'teacher*' 2>/dev/null | wc -l | tr -d ' ')
if [ "$leftover" -eq 0 ]; then
  echo "  PASS  drop table 清理 heap/index/WAL 文件"
  PASS=$((PASS + 1))
else
  echo "  FAIL  drop table 后仍残留 $leftover 个 teacher 文件"
  FAIL=$((FAIL + 1))
fi

echo
echo "e2e: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
