#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <set>
#include <string>

#include "sql/operator/hash_join_operator.h"
#include "sql/operator/relation_operators.h"
#include "sql/query_engine.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using Rows = std::vector<std::vector<Value>>;
using Keys = std::vector<HashJoinKey>;

class Source : public PhysicalOperator {
public:
  Source(Rows rows, int table) : rows_(std::move(rows)), table_(table) {}
  bool active = false;
  int opens = 0, closes = 0;
  void replace(Rows rows) { CHECK(!active); rows_ = std::move(rows); }
  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "composite_test_source"; }
  RC open() override { CHECK(!active); active = true; cursor_ = 0; ++opens; return RC::SUCCESS; }
  RC next(Tuple *&tuple) override {
    CHECK(active);
    if (cursor_ == rows_.size()) return RC::RECORD_EOF;
    row_.clear();
    const auto &values = rows_[cursor_++];
    for (size_t i = 0; i < values.size(); ++i) row_.append(values[i], ColumnRef(table_, static_cast<int>(i)));
    row_.rid = RID(1, static_cast<SlotNum>(cursor_ - 1)); row_.has_rid = true;
    tuple = &row_; return RC::SUCCESS;
  }
  RC close() override { CHECK(active); active = false; ++closes; return RC::SUCCESS; }
private:
  Rows rows_;
  int table_;
  size_t cursor_ = 0;
  Tuple row_;
};

class AllEqual : public Expression {
public:
  explicit AllEqual(Keys keys) : Expression(ExprType::COMPARISON), keys_(std::move(keys)) {}
  RC evaluate(const Tuple &tuple, Value &out) const override {
    for (const auto &key : keys_) {
      Value a, b;
      if (!tuple.get(key.left, a) || !tuple.get(key.right, b)) return RC::SCHEMA_FIELD_NOT_EXIST;
      int cmp = 0; RC rc = a.compare_to(b, cmp);
      if (OB_FAIL(rc)) return rc;
      if (cmp) { out = Value(false); return RC::SUCCESS; }
    }
    out = Value(true); return RC::SUCCESS;
  }
  std::string describe() const override { return "all_key_equalities"; }
private:
  Keys keys_;
};

class Capture : public ResultWriter {
public:
  Rows rows;
  RC write_header(const SqlResult &) override { rows.clear(); return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override {
    CHECK(tuple.values.size() == tuple.refs.size() && tuple.has_rid);
    rows.push_back(tuple.values); return RC::SUCCESS;
  }
  RC write_result(const SqlResult &) override { return RC::SUCCESS; }
};

static void children(PhysicalOperator &join, const Rows &left, const Rows &right, Source *&l, Source *&r) {
  auto a = std::make_unique<Source>(left, 0), b = std::make_unique<Source>(right, 1);
  l = a.get(); r = b.get(); join.add_child(std::move(a)); join.add_child(std::move(b));
}

static RC run(PhysicalOperator &plan, Capture &out) {
  QueryEngine engine(nullptr); engine.set_profile_enabled(true);
  SqlResult result; result.is_query = true;
  return engine.run_plan(plan, result, out);
}

static std::multiset<std::string> bag(const Rows &rows) {
  std::multiset<std::string> result;
  for (const auto &row : rows) {
    std::string bytes;
    for (const auto &v : row) {
      const auto text = v.to_string();
      bytes += std::to_string(static_cast<int>(v.type())) + ":" + std::to_string(text.size()) + ":" + text;
    }
    result.insert(bytes);
  }
  return result;
}

static Rows oracle(const Rows &left, const Rows &right, const Keys &keys) {
  Rows result;
  for (const auto &a : left) for (const auto &b : right) {
    bool equal = true;
    for (const auto &key : keys) {
      int cmp = 0; CHECK(a[key.left.column_index].compare_to(b[key.right.column_index], cmp) == RC::SUCCESS);
      if (cmp) { equal = false; break; }
    }
    if (equal) { auto row = a; row.insert(row.end(), b.begin(), b.end()); result.push_back(std::move(row)); }
  }
  return result;
}

static void compare(const Rows &left, const Rows &right, const Keys &keys) {
  HashJoinOperator hash(keys); AllEqual predicate(keys); NestedLoopJoinOperator nested(&predicate);
  Source *l, *r; children(hash, left, right, l, r);
  Source *nl, *nr; children(nested, left, right, nl, nr);
  const auto expected = bag(oracle(left, right, keys));
  for (int pass = 0; pass < 2; ++pass) {
    Capture a, b;
    CHECK(run(hash, a) == RC::SUCCESS && run(nested, b) == RC::SUCCESS);
    CHECK(bag(a.rows) == expected && bag(b.rows) == expected);
    CHECK(hash.statistics().candidate_pairs == static_cast<int64_t>(expected.size()));
    CHECK(nested.statistics().candidate_pairs == static_cast<int64_t>(left.size() * right.size()));
    CHECK(hash.statistics().build_rows == static_cast<int64_t>(left.size()));
    CHECK(hash.statistics().probe_rows == static_cast<int64_t>(left.empty() ? 0 : right.size()));
    CHECK(!l->active && !r->active && l->opens == l->closes && r->opens == r->closes);
    CHECK(hash.close() == RC::SUCCESS);
  }
}

static void correctness() {
  const Keys strings{{{0,0},{1,0},AttrType::CHARS}, {{0,1},{1,1},AttrType::CHARS}};
  const Rows text{{Value("a"),Value("bc")}, {Value("ab"),Value("c")},
                  {Value("a|b"),Value("c")}, {Value("a"),Value("b|c")},
                  {Value(std::string("a\0",2)),Value("b")}, {Value("a"),Value(std::string("\0b",2))},
                  {Value(""),Value("")}, {Value(""),Value("")}};
  compare(text, text, strings);
  compare({}, text, strings); compare(text, {}, strings); compare({}, {}, strings);
  const Keys mixed{{{0,0},{1,1},AttrType::INT}, {{0,2},{1,0},AttrType::BIGINT}, {{0,1},{1,2},AttrType::CHARS}};
  Rows left{{Value(-1),Value("x"),Value(int64_t(9007199254740992LL))},
            {Value(-1),Value("x"),Value(int64_t(9007199254740993LL))},
            {Value(-1),Value("x"),Value(int64_t(9007199254740993LL))}};
  Rows right{{Value(int64_t(9007199254740993LL)),Value(-1),Value("x")},
             {Value(int64_t(9007199254740993LL)),Value(-1),Value("x")}};
  compare(left, right, mixed); // Different column positions, mixed types, 2 x 2 duplicates.
  const Keys integers{{{0,0},{1,0},AttrType::INT}, {{0,1},{1,1},AttrType::INT}};
  std::mt19937 random(20261026);
  for (int trial = 0; trial < 50; ++trial) {
    Rows a, b;
    for (unsigned i = 0, n = random() % 25; i < n; ++i) a.push_back({Value(static_cast<int>(random()%3)),Value(static_cast<int>(random()%7))});
    for (unsigned i = 0, n = random() % 25; i < n; ++i) b.push_back({Value(static_cast<int>(random()%3)),Value(static_cast<int>(random()%7))});
    compare(a, b, integers);
  }
}

static void errors_and_budget() {
  const Keys keys{{{0,0},{1,0},AttrType::INT}, {{0,1},{1,1},AttrType::CHARS}};
  const Rows one{{Value(1),Value("abc")}};
  for (const Keys &invalid : {Keys{}, Keys{{{0,0},{1,0},AttrType::INT},{{0,-1},{1,1},AttrType::CHARS}}}) {
    HashJoinOperator join(invalid); Source *l, *r; children(join, one, one, l, r);
    CHECK(join.open() == RC::INVALID_ARGUMENT && l->opens == 0 && r->opens == 0);
  }
  HashJoinOperator unsupported(Keys{{{0,0},{1,0},AttrType::INT},{{0,1},{1,1},AttrType::FLOAT}});
  Source *l, *r; children(unsupported, one, one, l, r);
  CHECK(unsupported.open() == RC::UNSUPPORTED && l->opens == 0);
  for (bool probe : {false, true}) for (bool missing : {false, true}) {
    const Rows invalid = missing ? Rows{{Value(1)}} : Rows{{Value(1),Value(7)}};
    HashJoinOperator join(keys); children(join, probe ? one : invalid, probe ? invalid : one, l, r);
    Tuple *tuple = nullptr; RC rc = join.open();
    if (probe) { CHECK(rc == RC::SUCCESS); rc = join.next(tuple); }
    CHECK(rc == (missing ? RC::SCHEMA_FIELD_NOT_EXIST : RC::SCHEMA_FIELD_TYPE_MISMATCH));
    CHECK(!l->active && !r->active && join.close() == RC::SUCCESS);
    if (probe) r->replace(one); else l->replace(one);
    Capture out; CHECK(run(join, out) == RC::SUCCESS && out.rows.size() == 1);
  }
  HashJoinOperator single({0,0},{1,0},AttrType::INT), full(keys);
  children(single, one, one, l, r); children(full, one, one, l, r);
  Capture out; CHECK(run(single, out) == RC::SUCCESS && run(full, out) == RC::SUCCESS);
  CHECK(full.statistics().peak_build_bytes == single.statistics().peak_build_bytes + HashJoinOperator::KEY_PART_OVERHEAD + 3);
  const size_t budget = full.statistics().peak_build_bytes;
  HashJoinOperator limited(keys, nullptr, budget);
  children(limited, Rows{one[0],one[0]}, one, l, r); limited.reset_statistics(true);
  CHECK(limited.open() == RC::NOMEM && limited.statistics().build_rows == 1);
  CHECK(!l->active && !r->active && limited.close() == RC::SUCCESS);
  l->replace(one); CHECK(run(limited, out) == RC::SUCCESS && out.rows.size() == 1);
  HashJoinOperator early(keys); children(early, one, one, l, r); Tuple *tuple = nullptr;
  CHECK(early.open() == RC::SUCCESS && early.open() == RC::INVALID_ARGUMENT);
  CHECK(early.next(tuple) == RC::SUCCESS && early.close() == RC::SUCCESS && early.close() == RC::SUCCESS);
  CHECK(run(early, out) == RC::SUCCESS && out.rows.size() == 1);
}

static void demo() {
  Rows rows; for (int i = 0; i < 1000; ++i) rows.push_back({Value(1),Value(i)});
  const Keys keys{{{0,0},{1,0},AttrType::INT}, {{0,1},{1,1},AttrType::INT}};
  AllEqual residual(keys);
  HashJoinOperator single({0,0},{1,0},AttrType::INT,&residual), full(keys);
  Source *l, *r; children(single, rows, rows, l, r); children(full, rows, rows, l, r);
  Capture a, b; CHECK(run(single, a) == RC::SUCCESS && run(full, b) == RC::SUCCESS);
  CHECK(a.rows.size() == 1000 && bag(a.rows) == bag(b.rows));
  CHECK(single.statistics().candidate_pairs == 1000000 && full.statistics().candidate_pairs == 1000);
  std::puts("Composite Hash Join v6 demo: left=right=1000, same class_id, unique student_id");
  std::puts("Single-key hash + residual: candidates=1000000; output=1000");
  std::puts("Two-key hash:               candidates=1000; output=1000; result multisets equal: YES");
  std::puts("Profiles (inclusive times vary; peak_build_bytes_est is an estimate, not RSS):");
  single.explain_statistics(std::cout); full.explain_statistics(std::cout);
}

int main(int argc, char **argv) {
  correctness(); errors_and_budget();
  std::puts("Composite HashJoin v6: all tests passed (framing, mixed types, duplicates, random cases, lifecycle, budget)");
  if (argc == 2 && std::string(argv[1]) == "--demo") demo();
  return 0;
}
