// v5.2: real SQL -> optimizer -> Hash Join -> result/profile, including fallbacks.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "sql/query_engine.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using Rows = std::vector<std::vector<Value>>;

class Capture : public ResultWriter {
public:
  Rows rows;
  RC write_header(const SqlResult &) override { rows.clear(); return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override { rows.push_back(tuple.values); return RC::SUCCESS; }
  RC write_result(const SqlResult &) override { return RC::SUCCESS; }
};

static std::multiset<std::string> bag(const Rows &rows) {
  std::multiset<std::string> result;
  for (const auto &row : rows) {
    std::string key;
    for (const auto &v : row) {
      const std::string value = v.to_string();
      key += std::to_string(static_cast<int>(v.type())) + ":" + std::to_string(value.size()) + ":" + value;
    }
    result.insert(key);
  }
  return result;
}

class Fixture {
public:
  std::unique_ptr<Db> db;
  std::unique_ptr<QueryEngine> engine;
  Capture writer;
  Fixture() {
    const std::string root = "./build/test_hash_join_sql";
    CHECK(path_mkdir_recursive(root) == RC::SUCCESS);
    Db *raw = nullptr;
    RC rc = Db::open(root, "joins", &raw);
    if (rc == RC::SCHEMA_DB_NOT_EXIST || rc == RC::FILE_NOT_EXIST) rc = Db::create(root, "joins", &raw);
    CHECK(rc == RC::SUCCESS);
    db.reset(raw); engine = std::make_unique<QueryEngine>(db.get());
  }
  void sql(const std::string &text) {
    writer.rows.clear(); SqlResult result;
    RC rc = engine->execute(text, writer, result);
    if (OB_FAIL(rc)) std::fprintf(stderr, "SQL failed (%s): %s\n", strrc(rc), text.c_str());
    CHECK(rc == RC::SUCCESS && result.code == RC::SUCCESS);
  }
  void table(const std::string &name, const std::string &columns) {
    sql("drop table if exists " + name); sql("create table " + name + " (" + columns + ")");
  }
  std::string plan(const std::string &text, bool hash) {
    std::string explain;
    CHECK(engine->explain(text, explain) == RC::SUCCESS);
    const size_t physical = explain.find("== physical plan =="); CHECK(physical != std::string::npos);
    explain = explain.substr(physical);
    CHECK((explain.find("hash_join(") != std::string::npos) == hash);
    return explain;
  }
  void count(const std::string &text, int64_t expected, bool hash) {
    plan(text, hash);
    engine->set_profile_enabled(false); sql(text);
    CHECK(writer.rows.size() == 1 && writer.rows[0].size() == 1 && writer.rows[0][0].get_int64() == expected);
    CHECK(engine->last_profile().empty());
    engine->set_profile_enabled(true); sql(text);
    CHECK(writer.rows.size() == 1 && writer.rows[0][0].get_int64() == expected);
    CHECK((engine->last_profile().find("hash_join(") != std::string::npos) == hash);
    if (hash) {
      CHECK(engine->last_profile().find("build_rows=") != std::string::npos);
      CHECK(engine->last_profile().find("probe_rows=") != std::string::npos);
      CHECK(engine->last_profile().find("peak_build_bytes_est=") != std::string::npos);
    }
  }
};

static void basic_and_predicates(Fixture &f) {
  f.table("hj_a", "k int, v int"); f.table("hj_b", "k int, v int");
  f.sql("insert into hj_a values (1,10),(1,20),(2,30)");
  f.sql("insert into hj_b values (1,11),(1,21),(2,31),(9,99)");
  const std::string from = " from hj_a a, hj_b b ";
  f.count("select count(*)" + from + "where a.k=b.k", 5, true);
  CHECK(f.engine->last_profile().find("candidates=5 build_rows=3 probe_rows=4") != std::string::npos);
  const std::string select = "select a.k,a.v,b.k,b.v" + from + "where a.k=b.k";
  f.sql(select);
  const Rows expected{{Value(1),Value(10),Value(1),Value(11)}, {Value(1),Value(10),Value(1),Value(21)},
                      {Value(1),Value(20),Value(1),Value(11)}, {Value(1),Value(20),Value(1),Value(21)},
                      {Value(2),Value(30),Value(2),Value(31)}};
  CHECK(bag(f.writer.rows) == bag(expected));
  f.count("select count(*) from hj_b b, hj_a a where b.k=a.k", 5, true);
  f.count("select count(*)" + from + "where b.k=a.k and a.v<b.v", 4, true);
  f.count("select count(*)" + from + "where a.k=b.k and a.v>15 and b.v>15", 2, true);
  f.count("select count(*)" + from + "where a.k=b.k and (a.v=10 or b.v=31)", 3, true);
  f.count("select count(*)" + from + "where a.k=b.k and a.v=b.v", 0, true); // Second key must not be lost.
  f.count("select count(*)" + from + "where a.k=b.k or a.v=20", 7, false);
  f.count("select count(*)" + from + "where a.k<b.k", 5, false);
  f.count("select count(*)" + from, 12, false);
  f.count("select count(*) from hj_a a, hj_a b where a.k=b.k", 5, true);
  f.count("select count(*)" + from + "where a.k=b.k and 1=0", 0, false);
  f.count("select count(*)" + from + "where a.k=b.k and a.k<0", 0, true);
  f.table("hj_empty", "k int");
  f.count("select count(*) from hj_a a, hj_empty e where a.k=e.k", 0, true);
  CHECK(f.engine->last_profile().find("build_rows=0 probe_rows=0") != std::string::npos);
  f.count("select count(*) from hj_empty e, hj_empty r where e.k=r.k", 0, true);
  f.table("hj_none", "k int"); f.sql("insert into hj_none values (88),(99)");
  f.count("select count(*) from hj_none n, hj_a a where n.k=a.k", 0, true);
  f.sql("select a.k,count(*)" + from + "where a.k=b.k group by a.k");
  CHECK(bag(f.writer.rows) == bag(Rows{{Value(1),Value(int64_t(4))},{Value(2),Value(int64_t(1))}}));
  // Data changes followed by a new execution must not reuse an old hash table.
  f.sql("delete from hj_a where v=10");
  f.count("select count(*)" + from + "where a.k=b.k", 3, true);
}

static void types_and_multitable(Fixture &f) {
  f.table("hj_big_a", "k bigint"); f.table("hj_big_b", "k bigint");
  f.sql("insert into hj_big_a values (9007199254740992),(9007199254740993),(-9)");
  f.sql("insert into hj_big_b values (9007199254740993),(-9)");
  f.count("select count(*) from hj_big_a a,hj_big_b b where a.k=b.k", 2, true);
  f.table("hj_str_a", "k char(16)"); f.table("hj_str_b", "k char(32)");
  f.sql("insert into hj_str_a values (''),('x|y'),('x|y')");
  f.sql("insert into hj_str_b values (''),('x|y'),('x|y'),('z')");
  f.count("select count(*) from hj_str_a a,hj_str_b b where b.k=a.k", 5, true);
  f.table("hj_int", "k int"); f.table("hj_big", "k bigint"); f.table("hj_float", "k float");
  f.sql("insert into hj_int values (1),(2)"); f.sql("insert into hj_big values (1),(3)");
  f.sql("insert into hj_float values (1.0),(2.0)");
  f.count("select count(*) from hj_int a,hj_big b where a.k=b.k", 1, false);
  f.count("select count(*) from hj_float a,hj_float b where a.k=b.k", 2, false);
  f.count("select count(*) from hj_int a,hj_float b where a.k=b.k", 2, false);
  f.table("hj_c", "k int"); f.sql("insert into hj_c values (1),(1),(2),(7),(8)");
  f.count("select count(*) from hj_a a,hj_b b,hj_c c where a.k=b.k and b.k=c.k", 5, true);
  // A later input supplies a previously unavailable key. Do not attach it too early.
  f.count("select count(*) from hj_c c,hj_b b,hj_a a where a.k=c.k and b.k=c.k and a.v<b.v", 3, true);
  f.count("select count(*) from hj_a a,hj_b b,hj_c c where a.k=b.k and (a.k=c.k or c.k=8)", 8, true);
}

static void indexed_path_and_budget(Fixture &f) {
  f.table("hj_outer", "k int"); f.table("hj_indexed", "k int, primary key(k)");
  f.sql("insert into hj_outer values (1),(17)");
  std::string insert = "insert into hj_indexed values ";
  for (int i = 0; i < 100; ++i) insert += (i ? "," : "") + std::string("(") + std::to_string(i) + ")";
  f.sql(insert);
  const std::string query = "select count(*) from hj_outer a,hj_indexed b where a.k=b.k";
  CHECK(f.plan(query, false).find("join_lookup") != std::string::npos);
  f.count(query, 2, false);
  // Ordinary filtered index scans can be used by Hash Join; only a parameterized
  // join lookup is incompatible with its once-opened probe side.
  const std::string filtered = query + " and b.k<20";
  CHECK(f.plan(filtered, true).find("index_scan") != std::string::npos);
  f.count(filtered, 2, true);

  // The upper bound includes all earlier inputs, including wide CHAR payloads.
  // Actual matches are small, but the possible intermediate Cartesian product
  // exceeds 8 MiB, so the second join must retain a streaming NLJ fallback.
  for (const std::string name : {"hj_wide_a", "hj_wide_b", "hj_wide_c"}) {
    f.table(name, "k int, padding char(4000)");
    insert = "insert into " + name + " values ";
    const int count = name == "hj_wide_c" ? 33 : 32;
    for (int i = 0; i < count; ++i) insert += (i ? "," : "") + std::string("(") + std::to_string(i) + ",'')";
    f.sql(insert);
  }
  const std::string wide = "select count(*) from hj_wide_a a,hj_wide_b b,hj_wide_c c where a.k=b.k and b.k=c.k";
  const std::string plan = f.plan(wide, true);
  CHECK(plan.find("nested_loop_join") != std::string::npos);
  CHECK(plan.find("hash_join(", plan.find("hash_join(") + 1) == std::string::npos);
  f.count(wide, 32, true);
}

int main() {
  Fixture f;
  basic_and_predicates(f); types_and_multitable(f); indexed_path_and_budget(f);
  CHECK(f.db->close() == RC::SUCCESS);
  std::puts("HashJoin SQL v5.2: all tests passed (plans, profiles, predicates, types, multitable, indexes, budget)");
  return 0;
}
