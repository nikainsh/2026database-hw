/* minidb 性能测量夹具（只读使用生产代码的公开接口，不改动任何生产文件）
 *
 * 目的：为每一次优化提供「改前 / 改后」的可复现数字，并且用解析可得的期望行数
 *      保证优化没有改变查询结果（收益 = 更快且答案不变）。
 *
 * 用法：
 *   ./build/bin/bench_sql --data ~/minidb_bench --rows 200000 [--iters 5] [--quiet]
 *
 * 分阶段耗时：parse / bind(语义) / optimize(逻辑计划+规则) / physical(物理计划+选路) / execute
 * 端到端耗时：以上全部之和（每条查询取 iters 次的中位数）
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "sql/optimizer/optimizer.h"
#include "sql/parser/parser.h"
#include "sql/query_engine.h"
#include "sql/stmt/stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_us(Clock::time_point begin)
{
  return std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
}

/// 只统计行数与第一列之和，用于验证「结果不变」
class CountingWriter : public ResultWriter
{
public:
  RC write_header(const SqlResult &) override { return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override
  {
    ++rows_;
    if (!tuple.values.empty() && tuple.values[0].type() != AttrType::CHARS) {
      sum_ += tuple.values[0].type() == AttrType::FLOAT ? tuple.values[0].get_float()
                                                        : static_cast<double>(tuple.values[0].get_int64());
    }
    return RC::SUCCESS;
  }
  RC write_result(const SqlResult &result) override
  {
    code_ = result.code;
    return RC::SUCCESS;
  }

  int64_t rows() const { return rows_; }
  double  sum() const { return sum_; }
  RC      code() const { return code_; }

  void reset()
  {
    rows_ = 0;
    sum_  = 0;
    code_ = RC::SUCCESS;
  }

private:
  int64_t rows_ = 0;
  double  sum_  = 0;
  RC      code_ = RC::SUCCESS;
};

struct BenchQuery
{
  const char *name;
  const char *sql;
  int64_t     expect_rows;  /// 解析可得的期望值，用来同时保证正确性
  double      expect_sum;
};

/* 数据集（确定性生成，无随机）：
 *   bench(id int 主键, cat int, val int, name char(16)) 共 N 行
 *     id   = i            -> 唯一，走主键索引
 *     cat  = i % 100       -> 100 个不同值，每个 N/100 行（选择率 1%）
 *     val  = i % 5000      -> 5000 个不同值，每个 N/5000 行（选择率 0.02%）
 *     name = "n<i>"         -> 无索引
 *   索引：cat、val 各一个非唯一索引 + 主键自动唯一索引
 *   dim(k int, g int) 共 M=10 行：k = 0..9，g = k % 10
 */
constexpr int64_t kDistinctCat = 100;
constexpr int64_t kDistinctVal = 5000;
constexpr int64_t kDimRows     = 10;

std::vector<BenchQuery> make_queries(int64_t n)
{
  const int64_t cat_rows  = n / kDistinctCat;   /// 每个 cat 值的行数（选择率 1%）
  const int64_t val_rows  = n / kDistinctVal;   /// 每个 val 值的行数（选择率 0.02%）
  const int64_t all_rows  = n;

  return {
      {"eq_pk", "select id, cat from bench where id = 1000", 1, 1000},
      {"range_pk_narrow", "select count(*) from bench where id >= 1000 and id < 1100", 1, 100},
      {"eq_val_low_sel", "select count(*) from bench where val = 1234", 1, val_rows},
      {"eq_cat_1pct", "select count(*) from bench where cat = 7", 1, static_cast<double>(cat_rows)},
      {"range_val_low_sel", "select count(*) from bench where val < 100", 1, 100.0 * val_rows},
      {"range_cat_all", "select count(*) from bench where cat >= 0", 1, static_cast<double>(all_rows)},
      {"two_indexed_cols", "select count(*) from bench where id > 2 and cat < 4", 1, 4 * cat_rows - 3.0},
      {"no_index_col", "select count(*) from bench where name = 'n12345'", 1, 1},
      {"count_all", "select count(*) from bench", 1, static_cast<double>(n)},
      {"group_by_cat", "select cat, count(*) from bench group by cat", kDistinctCat, 4950.0},
      {"join_on_pk", "select count(*) from bench, dim where bench.id = dim.k", 1, kDimRows},
      {"join_on_cat", "select count(*) from bench, dim where bench.cat = dim.g", 1, static_cast<double>(cat_rows * kDimRows)},
      {"project_wide", "select id, cat, val, name from bench where val < 10", val_rows * 10, 0},
  };
}

struct StageTime
{
  double parse = 0, bind = 0, optimize = 0, physical = 0, execute = 0, total = 0;
};

class Runner
{
public:
  Runner(Db *db) : db_(db) {}

  /// 返回该查询一次完整编译+执行的分阶段耗时；同时回填行数与校验和
  RC run_once(const char *sql, StageTime &out, CountingWriter &writer, std::string &plan_text)
  {
    std::unique_ptr<ParseResult> parse_result;
    const Clock::time_point t_all = Clock::now();

    Clock::time_point t0 = Clock::now();
    Parser            parser;
    RC                rc = parser.parse_statement(sql, parse_result);
    out.parse += elapsed_us(t0);
    if (OB_FAIL(rc)) {
      out.total += elapsed_us(t_all);
      return rc;
    }

    t0 = Clock::now();
    std::unique_ptr<Stmt> stmt;
    rc = Stmt::create_stmt(*parse_result, db_, stmt);
    out.bind += elapsed_us(t0);
    if (OB_FAIL(rc)) {
      out.total += elapsed_us(t_all);
      return rc;
    }

    t0 = Clock::now();
    LogicalPlan plan;
    rc = LogicalPlanGenerator().create(*stmt, plan);
    if (OB_FAIL(rc)) {
      out.optimize += elapsed_us(t0);
      out.total += elapsed_us(t_all);
      return rc;
    }
    Optimizer optimizer(plan);
    rc = optimizer.optimize();
    out.optimize += elapsed_us(t0);
    if (OB_FAIL(rc)) {
      out.total += elapsed_us(t_all);
      return rc;
    }

    t0 = Clock::now();
    std::unique_ptr<PhysicalOperator> physical;
    rc = PhysicalPlanGenerator().create(*plan.root, physical);
    out.physical += elapsed_us(t0);
    if (OB_FAIL(rc)) {
      out.total += elapsed_us(t_all);
      return rc;
    }
    if (capture_plan_ && plan_text.empty()) {
      std::ostringstream oss;
      physical->explain(oss);
      plan_text = oss.str();
    }

    t0 = Clock::now();
    SqlResult result;
    result.is_query = true;
    writer.reset();
    rc = QueryEngine(db_).run_plan(*physical, result, writer);
    out.execute += elapsed_us(t0);

    out.total += elapsed_us(t_all);
    return rc;
  }

  void enable_plan_capture() { capture_plan_ = true; }

private:
  Db  *db_ = nullptr;
  bool capture_plan_ = false;
};

int64_t median(std::vector<double> v)
{
  if (v.empty()) {
    return 0;
  }
  std::sort(v.begin(), v.end());
  return static_cast<int64_t>(v[v.size() / 2]);
}

RC build_dataset(Db *db, int64_t rows)
{
  std::vector<ColumnMeta> columns;
  ColumnMeta id, cat, val, name;
  RC         rc = id.init("id", AttrType::INT, 0, 0);
  if (OB_FAIL(rc)) return rc;
  rc = cat.init("cat", AttrType::INT, 0, 0);
  if (OB_FAIL(rc)) return rc;
  rc = val.init("val", AttrType::INT, 0, 0);
  if (OB_FAIL(rc)) return rc;
  rc = name.init("name", AttrType::CHARS, 16, 0);
  if (OB_FAIL(rc)) return rc;
  columns = {id, cat, val, name};

  TableMeta meta;
  rc = meta.init("bench", columns);
  if (OB_FAIL(rc)) return rc;
  rc = db->create_table(meta);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_TABLE_EXIST) return rc;

  std::vector<ColumnMeta> dim_columns;
  ColumnMeta              k, g;
  k.init("k", AttrType::INT, 0, 0);
  g.init("g", AttrType::INT, 0, 0);
  dim_columns = {k, g};
  TableMeta   dim_meta;
  rc = dim_meta.init("dim", dim_columns);
  if (OB_FAIL(rc)) return rc;
  rc = db->create_table(dim_meta);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_TABLE_EXIST) return rc;

  Table *bench = nullptr;
  rc = db->find_table("bench", &bench);
  if (OB_FAIL(rc)) return rc;

  const Clock::time_point begin = Clock::now();
  for (int64_t i = 0; i < rows; ++i) {
    std::vector<Value> values;
    values.push_back(Value(static_cast<int32_t>(i)));
    values.push_back(Value(static_cast<int32_t>(i % kDistinctCat)));
    values.push_back(Value(static_cast<int32_t>(i % kDistinctVal)));
    values.push_back(Value("n" + std::to_string(i)));

    Record record;
    rc   = bench->table_meta().make_record(values, record);
    if (OB_FAIL(rc)) return rc;
    rc   = bench->insert_record(record);
    if (OB_FAIL(rc)) {
      printf("insert failed at %lld: %s\n", (long long)i, strrc(rc));
      return rc;
    }
  }

  Table *dim = nullptr;
  rc = db->find_table("dim", &dim);
  if (OB_FAIL(rc)) return rc;
  for (int64_t i = 0; i < kDimRows; ++i) {
    std::vector<Value> values;
    values.push_back(Value(static_cast<int32_t>(i)));
    values.push_back(Value(static_cast<int32_t>(i % kDistinctCat)));
    Record record;
    rc   = dim->table_meta().make_record(values, record);
    if (OB_FAIL(rc)) return rc;
    rc   = dim->insert_record(record);
    if (OB_FAIL(rc)) return rc;
  }
  printf("dataset built: bench=%lld dim=%lld (%.1f s)\n", (long long)rows, (long long)kDimRows,
         elapsed_us(begin) / 1e6);

  IndexMeta idx_id;
  idx_id.index_name = "bench_id_idx";
  idx_id.field_name = "id";
  idx_id.unique     = true;  /// 模拟真实场景里主键自动建的唯一索引
  rc = db->create_index("bench", idx_id);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_INDEX_NAME_REPEAT) return rc;

  IndexMeta idx_cat;
  idx_cat.index_name = "bench_cat_idx";
  idx_cat.field_name = "cat";
  idx_cat.unique     = false;
  rc = db->create_index("bench", idx_cat);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_INDEX_NAME_REPEAT) return rc;

  IndexMeta idx_val;
  idx_val.index_name = "bench_val_idx";
  idx_val.field_name = "val";
  idx_val.unique     = false;
  rc = db->create_index("bench", idx_val);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_INDEX_NAME_REPEAT) return rc;

  IndexMeta dim_k;
  dim_k.index_name = "dim_k_idx";
  dim_k.field_name = "k";
  dim_k.unique     = true;
  rc = db->create_index("dim", dim_k);
  if (OB_FAIL(rc) && rc != RC::SCHEMA_INDEX_NAME_REPEAT) return rc;

  return db->flush();
}

void usage()
{
  printf("usage: bench_sql [--data DIR] [--db NAME] [--rows N] [--iters K] [--plan] [--quiet] [--build-only]\n");
}

}  // namespace

int main(int argc, char **argv)
{
  std::string data_dir = getenv("HOME") ? std::string(getenv("HOME")) + "/minidb_bench" : std::string("./bench_data");
  std::string db_name  = "bench";
  int64_t     rows     = 200000;
  int         iters    = 7;
  bool        show_plan = false;
  bool        quiet     = false;
  bool        build_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data" && i + 1 < argc) { data_dir = argv[++i]; }
    else if (arg == "--db" && i + 1 < argc) { db_name = argv[++i]; }
    else if (arg == "--rows" && i + 1 < argc) { rows = atoll(argv[++i]); }
    else if (arg == "--iters" && i + 1 < argc) { iters = atoi(argv[++i]); }
    else if (arg == "--plan") { show_plan = true; }
    else if (arg == "--quiet") { quiet = true; }
    else if (arg == "--build-only") { build_only = true; }
    else { usage(); return 2; }
  }
  log_set_level(LogLevel::ERR);

  path_mkdir_recursive(data_dir);
  const std::string db_with_rows = db_name + "_" + std::to_string(rows);

  Db *db = nullptr;
  RC  rc = Db::open(data_dir, db_with_rows, &db);
  if (rc == RC::SCHEMA_DB_NOT_EXIST || rc == RC::FILE_NOT_EXIST) {
    rc = Db::create(data_dir, db_with_rows, &db);
    if (OB_FAIL(rc)) { printf("cannot create db: %s\n", strrc(rc)); return 1; }
    rc = build_dataset(db, rows);
    if (OB_FAIL(rc)) { printf("cannot build dataset: %s\n", strrc(rc)); return 1; }
    delete db;
    db = nullptr;
    rc = Db::open(data_dir, db_with_rows, &db);
    if (OB_FAIL(rc)) { printf("cannot reopen db: %s\n", strrc(rc)); return 1; }
  } else if (OB_FAIL(rc)) {
    printf("cannot open db: %s\n", strrc(rc));
    return 1;
  } else {
    Table *bench = nullptr;
    if (OB_FAIL(db->find_table("bench", &bench)) || bench == nullptr) {
      printf("dataset missing, please remove %s/%s and rerun\n", data_dir.c_str(), db_with_rows.c_str());
      return 1;
    }
    int64_t actual = 0;
    bench->record_count(actual);
    if (actual != rows) {
      printf("dataset row count mismatch: got %lld expect %lld (remove %s/%s and rerun)\n",
             (long long)actual, (long long)rows, data_dir.c_str(), db_with_rows.c_str());
      return 1;
    }
    printf("dataset reused: %s/%s rows=%lld\n", data_dir.c_str(), db_with_rows.c_str(), (long long)rows);
  }

  if (build_only) {
    delete db;
    return 0;
  }

  Runner    runner(db);
  Runner    plan_runner(db);
  plan_runner.enable_plan_capture();  /// 只有取计划文本的那一次会付 explain 的字符串构造代价
  auto      queries = make_queries(rows);
  int       failures = 0;
  StageTime total;

  printf("== bench: db=%s rows=%lld iters=%d ==\n", db_with_rows.c_str(), (long long)rows, iters);
  printf("%-18s %9s %9s %9s %9s %10s %10s %8s %s\n", "query", "parse", "bind", "optimize", "physical",
         "execute", "total(us)", "rows", "check");
  printf("%-18s %9s %9s %9s %9s %10s %10s %8s %s\n", "", "(us)", "(us)", "(us)", "(us)", "(us)", "(us)", "", "");

  for (const BenchQuery &q : queries) {
    std::vector<double> parse, bind, opt, phys, exec, tot;
    int64_t             rows_got = -1;
    double              sum_got  = 0;
    RC                  last     = RC::SUCCESS;
    std::string         plan_text;

    if (show_plan) {  /// 单独一次不计时的运行，只为抓物理计划文本
      StageTime      dummy;
      CountingWriter plan_writer;
      plan_runner.run_once(q.sql, dummy, plan_writer, plan_text);
    }

    for (int i = 0; i < iters; ++i) {
      StageTime      st;
      CountingWriter writer;
      std::string    text;
      st.parse = st.bind = st.optimize = st.physical = st.execute = st.total = 0;
      last = runner.run_once(q.sql, st, writer, text);
      if (OB_FAIL(last)) {
        break;
      }
      parse.push_back(st.parse);
      bind.push_back(st.bind);
      opt.push_back(st.optimize);
      phys.push_back(st.physical);
      exec.push_back(st.execute);
      tot.push_back(st.total);
      rows_got = writer.rows();
      sum_got  = writer.sum();
    }

    const bool rows_ok = (rows_got == q.expect_rows);
    const bool sum_ok  = !rows_ok || std::fabs(sum_got - q.expect_sum) < 1e-6 || q.expect_sum == 0;
    if (OB_FAIL(last) || !rows_ok || !sum_ok) {
      failures++;
    }

    std::string check;
    if (OB_FAIL(last)) {
      check = std::string("ERR:") + strrc(last);
    } else if (rows_ok && sum_ok) {
      check = "ok";
    } else {
      check = "MISMATCH expect_rows=" + std::to_string(q.expect_rows) + " got=" + std::to_string(rows_got) +
              " expect_sum=" + std::to_string(q.expect_sum) + " got_sum=" + std::to_string(sum_got);
    }

    printf("%-18s %9lld %9lld %9lld %9lld %10lld %10lld %8lld %s\n", q.name,
           (long long)median(parse), (long long)median(bind), (long long)median(opt), (long long)median(phys),
           (long long)median(exec), (long long)median(tot), (long long)rows_got, check.c_str());

    total.parse += static_cast<double>(median(parse));
    total.bind += static_cast<double>(median(bind));
    total.optimize += static_cast<double>(median(opt));
    total.physical += static_cast<double>(median(phys));
    total.execute += static_cast<double>(median(exec));
    total.total += static_cast<double>(median(tot));

    if (show_plan && !plan_text.empty() && !quiet) {
      printf("  plan[%s]:\n%s", q.name, plan_text.c_str());
    }
  }

  printf("%-18s %9.0f %9.0f %9.0f %9.0f %10.0f %10.0f %8s %s\n", "TOTAL(sum)", total.parse, total.bind,
         total.optimize, total.physical, total.execute, total.total, "", "");
  printf("compile-only total = %.0f us\n", total.parse + total.bind + total.optimize + total.physical);

  delete db;
  if (failures > 0) {
    printf("BENCH FAILURES = %d\n", failures);
    return 1;
  }
  return 0;
}
