// Operator-only v5.1: SQL plan selection is deliberately left to the planner.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>
#include <sstream>

#include "sql/operator/hash_join_operator.h"
#include "sql/operator/relation_operators.h"
#include "sql/query_engine.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using Rows = std::vector<std::vector<Value>>;

class Source : public PhysicalOperator {
public:
  Source(Rows rows, int table) : rows_(std::move(rows)), table_(table) {}
  RC open_error = RC::SUCCESS, read_error = RC::SUCCESS, close_error = RC::SUCCESS;
  bool active = false, null_row = false;
  int opens = 0, closes = 0;
  void replace_rows(Rows rows) { CHECK(!active); rows_ = std::move(rows); }
  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "test_source"; }
  RC open() override {
    CHECK(!active); ++opens;
    if (OB_FAIL(open_error)) return open_error;
    active = true; cursor_ = 0; return RC::SUCCESS;
  }
  RC next(Tuple *&tuple) override {
    CHECK(active);
    if (OB_FAIL(read_error)) return read_error;
    if (null_row) { tuple = nullptr; return RC::SUCCESS; }
    if (cursor_ == rows_.size()) return RC::RECORD_EOF;
    row_.clear();
    const auto &values = rows_[cursor_++];
    for (size_t i = 0; i < values.size(); ++i) row_.append(values[i], ColumnRef(table_, static_cast<int>(i)));
    row_.rid = RID(9, static_cast<SlotNum>(cursor_ - 1)); row_.has_rid = true;
    tuple = &row_; return RC::SUCCESS;
  }
  RC close() override { CHECK(active); active = false; ++closes; return close_error; }
private:
  Rows rows_;
  int table_;
  size_t cursor_ = 0;
  Tuple row_; // Reused on every next: join must copy retained rows.
};

class Equality : public Expression {
public:
  Equality() : Expression(ExprType::COMPARISON) {}
  RC evaluate(const Tuple &tuple, Value &out) const override {
    Value a, b;
    if (!tuple.get(ColumnRef(0, 0), a) || !tuple.get(ColumnRef(1, 0), b)) return RC::SCHEMA_FIELD_NOT_EXIST;
    int comparison = 0; RC rc = a.compare_to(b, comparison);
    if (OB_SUCC(rc)) out = Value(comparison == 0);
    return rc;
  }
  std::string describe() const override { return "left.key=right.key"; }
};

class Residual : public Expression {
public:
  Residual() : Expression(ExprType::COMPARISON) {}
  RC error = RC::SUCCESS;
  bool wrong_type = false;
  RC evaluate(const Tuple &tuple, Value &out) const override {
    if (OB_FAIL(error)) return error;
    Value l, r;
    CHECK(tuple.get(ColumnRef(0, 1), l) && tuple.get(ColumnRef(1, 1), r));
    out = wrong_type ? Value(1) : Value(l.get_int32() < r.get_int32());
    return RC::SUCCESS;
  }
  std::string describe() const override { return "left.payload<right.payload"; }
};

class Capture : public ResultWriter {
public:
  Rows rows;
  RC write_header(const SqlResult &) override { rows.clear(); return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override {
    CHECK(tuple.values.size() == 4 && tuple.refs.size() == 4);
    Value value;
    CHECK(tuple.get(ColumnRef(0, 1), value));
    CHECK(tuple.has_rid);
    rows.push_back(tuple.values); return RC::SUCCESS;
  }
  RC write_result(const SqlResult &) override { return RC::SUCCESS; }
};

// Length/type framing avoids a display-based oracle losing embedded NUL or BIGINT precision.
static std::multiset<std::string> bag(const Rows &rows) {
  std::multiset<std::string> result;
  for (const auto &row : rows) {
    std::string key;
    for (const auto &value : row) {
      std::string data = value.type() == AttrType::CHARS ? value.get_string() : value.to_string();
      key += std::to_string(static_cast<int>(value.type())) + ":" + std::to_string(data.size()) + ":" + data;
    }
    result.insert(key);
  }
  return result;
}

static Rows oracle(const Rows &left, const Rows &right, bool residual = false) {
  Rows result;
  for (const auto &l : left) for (const auto &r : right) {
    int comparison = 0; CHECK(l[0].compare_to(r[0], comparison) == RC::SUCCESS);
    if (comparison != 0 || (residual && l[1].get_int32() >= r[1].get_int32())) continue;
    auto row = l; row.insert(row.end(), r.begin(), r.end()); result.push_back(std::move(row));
  }
  return result;
}

static void children(PhysicalOperator &join, const Rows &left, const Rows &right, Source *&l, Source *&r) {
  auto a = std::make_unique<Source>(left, 0), b = std::make_unique<Source>(right, 1);
  l = a.get(); r = b.get(); join.add_child(std::move(a)); join.add_child(std::move(b));
}

static RC run(PhysicalOperator &plan, Capture &writer, QueryEngine &engine) {
  SqlResult result; result.is_query = true; return engine.run_plan(plan, result, writer);
}

static void compare(const Rows &left, const Rows &right, AttrType type) {
  HashJoinOperator hash(ColumnRef(0, 0), ColumnRef(1, 0), type);
  Equality eq; NestedLoopJoinOperator nested(&eq);
  Source *hl, *hr, *nl, *nr;
  children(hash, left, right, hl, hr); children(nested, left, right, nl, nr);
  QueryEngine engine(nullptr); engine.set_profile_enabled(true);
  const auto expected = bag(oracle(left, right));
  for (int pass = 0; pass < 2; ++pass) {
    Capture a, b;
    CHECK(run(hash, a, engine) == RC::SUCCESS);
    CHECK(run(nested, b, engine) == RC::SUCCESS);
    CHECK(bag(a.rows) == expected && bag(b.rows) == expected);
    CHECK(hash.statistics().build_rows == static_cast<int64_t>(left.size()));
    CHECK(hash.statistics().probe_rows == static_cast<int64_t>(left.empty() ? 0 : right.size()));
    CHECK(hash.statistics().candidate_pairs == static_cast<int64_t>(expected.size()));
    CHECK(nested.statistics().candidate_pairs == static_cast<int64_t>(left.size() * right.size()));
    CHECK(hr->statistics().open_calls == (left.empty() ? 0 : 1));
    CHECK(!hl->active && !hr->active && !nl->active && !nr->active);
    CHECK(hash.close() == RC::SUCCESS);
  }
}

static void correctness() {
  const Rows a{{Value(1), Value(10)}, {Value(1), Value(20)}, {Value(2), Value(30)}};
  const Rows b{{Value(1), Value(11)}, {Value(1), Value(21)}, {Value(3), Value(31)}};
  compare(a, b, AttrType::INT); // 2 x 2 duplicates must yield FOUR, not two.
  compare({}, b, AttrType::INT); compare(a, {}, AttrType::INT); compare({}, {}, AttrType::INT);
  compare({{Value(-8), Value(0)}}, {{Value(8), Value(0)}}, AttrType::INT);
  Rows ints;
  for (int key : {std::numeric_limits<int>::min(), -1, 0, std::numeric_limits<int>::max()}) ints.push_back({Value(key), Value(0)});
  compare(ints, ints, AttrType::INT);
  Rows big;
  for (int64_t key : {std::numeric_limits<int64_t>::min(), int64_t(9007199254740992LL),
                     int64_t(9007199254740993LL), std::numeric_limits<int64_t>::max()}) big.push_back({Value(key), Value(0)});
  compare(big, big, AttrType::BIGINT);
  Rows strings;
  for (const auto &key : {std::string(""), std::string("a|b"), std::string("a"), std::string("a\0b", 3), std::string("a\0", 2)})
    strings.push_back({Value(key), Value(0)});
  compare(strings, strings, AttrType::CHARS);
  std::mt19937 rng(20261025);
  for (int trial = 0; trial < 40; ++trial) {
    Rows l, r;
    for (unsigned i = 0, n = rng() % 25; i < n; ++i) l.push_back({Value(static_cast<int>(rng() % 7) - 3), Value(static_cast<int>(i))});
    for (unsigned i = 0, n = rng() % 25; i < n; ++i) r.push_back({Value(static_cast<int>(rng() % 7) - 3), Value(static_cast<int>(i))});
    compare(l, r, AttrType::INT);
  }
  Residual residual;
  HashJoinOperator join(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT, &residual);
  Source *l, *r; children(join, a, b, l, r);
  QueryEngine engine(nullptr); Capture out;
  CHECK(run(join, out, engine) == RC::SUCCESS && bag(out.rows) == bag(oracle(a, b, true)));
  // Keys need not occupy column zero; output stays left-then-right with original refs.
  HashJoinOperator nonfirst(ColumnRef(0, 1), ColumnRef(1, 1), AttrType::INT);
  children(nonfirst, Rows{{Value(7), Value(1)}}, Rows{{Value(8), Value(1)}}, l, r);
  CHECK(run(nonfirst, out, engine) == RC::SUCCESS);
  CHECK(bag(out.rows) == bag(Rows{{Value(7), Value(1), Value(8), Value(1)}}));
}

static void lifecycle_and_errors() {
  const Rows one{{Value(1), Value(0)}};
  HashJoinOperator join(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT);
  Source *l, *r; children(join, one, one, l, r);
  Tuple *tuple = nullptr;
  CHECK(join.next(tuple) == RC::INTERNAL);
  CHECK(join.close() == RC::SUCCESS);
  CHECK(join.open() == RC::SUCCESS && join.open() == RC::INVALID_ARGUMENT);
  CHECK(join.next(tuple) == RC::SUCCESS && tuple->rid.slot_num == 0);
  CHECK(join.close() == RC::SUCCESS && join.close() == RC::SUCCESS); // Early consumer stop.
  CHECK(join.next(tuple) == RC::INTERNAL);
  QueryEngine engine(nullptr); Capture out;
  CHECK(run(join, out, engine) == RC::SUCCESS && out.rows.size() == 1);
  for (int side = 0; side < 2; ++side) {
    Source *source = side ? r : l;
    int old_closes = source->closes;
    source->open_error = RC::IOERR_OPEN;
    CHECK(join.open() == RC::IOERR_OPEN && source->closes == old_closes);
    CHECK(!l->active && !r->active);
    source->open_error = RC::SUCCESS;
    source->read_error = RC::IOERR_READ;
    RC rc = join.open();
    if (side) { CHECK(rc == RC::SUCCESS); rc = join.next(tuple); }
    CHECK(rc == RC::IOERR_READ && !l->active && !r->active);
    source->read_error = RC::SUCCESS;
    CHECK(run(join, out, engine) == RC::SUCCESS && out.rows.size() == 1);
    source->null_row = true;
    rc = join.open(); if (side) { CHECK(rc == RC::SUCCESS); rc = join.next(tuple); }
    CHECK(rc == RC::INTERNAL && !l->active && !r->active);
    source->null_row = false;
    source->close_error = RC::IOERR_CLOSE;
    rc = join.open();
    if (side) { CHECK(rc == RC::SUCCESS); rc = join.close(); }
    CHECK(rc == RC::IOERR_CLOSE && !l->active && !r->active);
    source->close_error = RC::SUCCESS;
  }
  Residual residual;
  HashJoinOperator filtered(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT, &residual);
  children(filtered, one, one, l, r);
  residual.error = RC::IOERR_READ; r->close_error = RC::IOERR_CLOSE;
  CHECK(filtered.open() == RC::SUCCESS && filtered.next(tuple) == RC::IOERR_READ); // Original error wins.
  CHECK(!l->active && !r->active && filtered.close() == RC::SUCCESS);
  residual.error = RC::SUCCESS; r->close_error = RC::SUCCESS; residual.wrong_type = true;
  CHECK(filtered.open() == RC::SUCCESS && filtered.next(tuple) == RC::SCHEMA_FIELD_TYPE_MISMATCH);
  residual.wrong_type = false;
  CHECK(run(filtered, out, engine) == RC::SUCCESS && out.rows.empty());

  for (bool right : {false, true}) {
    for (bool missing : {false, true}) {
      Rows invalid = missing ? Rows{{}} : Rows{{Value("bad"), Value(0)}};
      HashJoinOperator bad(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT);
      children(bad, right ? one : invalid, right ? invalid : one, l, r);
      RC rc = bad.open(); if (right) { CHECK(rc == RC::SUCCESS); rc = bad.next(tuple); }
      CHECK(rc == (missing ? RC::SCHEMA_FIELD_NOT_EXIST : RC::SCHEMA_FIELD_TYPE_MISMATCH));
      CHECK(!l->active && !r->active && bad.close() == RC::SUCCESS);
    }
  }
  HashJoinOperator unsupported(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::FLOAT);
  children(unsupported, one, one, l, r);
  CHECK(unsupported.open() == RC::UNSUPPORTED && l->opens == 0 && r->opens == 0);
  HashJoinOperator incomplete(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT);
  CHECK(incomplete.open() == RC::INTERNAL);
  HashJoinOperator invalid_ref(ColumnRef(-1, 0), ColumnRef(1, 0), AttrType::INT);
  children(invalid_ref, one, one, l, r); CHECK(invalid_ref.open() == RC::INVALID_ARGUMENT);
  for (size_t budget : {size_t(0), size_t(1), size_t(300)}) {
    HashJoinOperator limited(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT, nullptr, budget);
    children(limited, Rows{{Value(1), Value(std::string(4096, 'x'))}}, one, l, r);
    for (int pass = 0; pass < 2; ++pass) {
      CHECK(limited.open() == RC::NOMEM && !l->active && r->opens == 0);
      CHECK(limited.close() == RC::SUCCESS);
    }
  }
  // Exceed budget after some rows are already retained, then recover on same instance.
  HashJoinOperator measured(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT);
  children(measured, one, one, l, r); measured.reset_statistics(true);
  CHECK(run(measured, out, engine) == RC::SUCCESS);
  // run_plan resets statistics according to the engine setting.
  engine.set_profile_enabled(true); CHECK(run(measured, out, engine) == RC::SUCCESS);
  const size_t row_budget = measured.statistics().peak_build_bytes;
  CHECK(row_budget > 0);
  HashJoinOperator limited(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT, nullptr, row_budget);
  children(limited, Rows{one[0], one[0]}, one, l, r);
  limited.reset_statistics(true);
  CHECK(limited.open() == RC::NOMEM && limited.statistics().build_rows == 1);
  CHECK(!l->active && !r->active && limited.close() == RC::SUCCESS);
  l->replace_rows(one);
  CHECK(run(limited, out, engine) == RC::SUCCESS && out.rows.size() == 1);
  CHECK(limited.statistics().build_rows == 1 && limited.statistics().probe_rows == 1);
  // Empty build needs no storage, even with a zero budget.
  HashJoinOperator empty(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT, nullptr, 0);
  children(empty, {}, one, l, r); r->open_error = RC::IOERR_OPEN;
  CHECK(empty.open() == RC::SUCCESS && empty.next(tuple) == RC::RECORD_EOF && r->opens == 0);
  CHECK(empty.next(tuple) == RC::RECORD_EOF && empty.close() == RC::SUCCESS);
}

static void demo() {
  Rows rows;
  for (int i = 0; i < 1000; ++i) rows.push_back({Value(i), Value(i)});
  HashJoinOperator hash(ColumnRef(0, 0), ColumnRef(1, 0), AttrType::INT);
  Equality eq; NestedLoopJoinOperator nested(&eq);
  Source *l, *r; children(hash, rows, rows, l, r); children(nested, rows, rows, l, r);
  QueryEngine engine(nullptr); engine.set_profile_enabled(true); Capture a, b;
  CHECK(run(nested, a, engine) == RC::SUCCESS); const auto nested_profile = engine.last_profile();
  CHECK(run(hash, b, engine) == RC::SUCCESS);
  CHECK(a.rows.size() == 1000 && bag(a.rows) == bag(b.rows));
  CHECK(nested.statistics().candidate_pairs == 1000000 && hash.statistics().candidate_pairs == 1000);
  CHECK(nested.children()[1]->statistics().open_calls == 1000);
  CHECK(hash.children()[1]->statistics().open_calls == 1);
  std::puts("Hash Join v5.1 operator demo (prepared plans, NOT SQL optimizer selection)");
  std::puts("Input: left=1000 right=1000 unique INT keys; result multisets equal: YES (1000 rows)");
  std::puts("Nested loop: candidates=1000000; right opens=1000");
  std::puts("Hash join:   candidates=1000; build_rows=1000; probe_rows=1000; right opens=1");
  std::puts("Profiles (time varies; estimated build bytes are NOT process RSS):");
  std::printf("%s%s", nested_profile.c_str(), engine.last_profile().c_str());
}

int main(int argc, char **argv) {
  correctness(); lifecycle_and_errors();
  std::puts("HashJoin v5.1: all tests passed (correctness, duplicates, random cases, lifecycle, errors, budget)");
  if (argc == 2 && std::string(argv[1]) == "--demo") demo();
  return 0;
}
