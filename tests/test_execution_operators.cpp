// Combined v3/v4: real filter/project/aggregate correctness and execution counters.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "sql/operator/aggregate_operators.h"
#include "sql/operator/relation_operators.h"
#include "sql/query_engine.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
  std::exit(1); } } while (false)

using Rows = std::vector<std::vector<Value>>;

class RowSource : public PhysicalOperator
{
public:
  explicit RowSource(Rows rows, int table = 0) : rows_(std::move(rows)), table_(table) {}
  int opens = 0, closes = 0;
  bool active = false;
  void replace_rows(Rows rows) { CHECK(!active); rows_ = std::move(rows); }
  RC read_error = RC::SUCCESS;
  RC close_error = RC::SUCCESS;
  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "test_rows"; }
  RC open() override { CHECK(!active); active = true; ++opens; cursor_ = 0; return RC::SUCCESS; }
  RC next(Tuple *&tuple) override
  {
    CHECK(active);
    if (OB_FAIL(read_error)) return read_error;
    if (cursor_ == rows_.size()) return RC::RECORD_EOF;
    row_.clear();
    const auto &values = rows_[cursor_++];
    for (size_t i = 0; i < values.size(); ++i) row_.append(values[i], ColumnRef(table_, static_cast<int>(i)));
    row_.rid = RID(2, static_cast<SlotNum>(cursor_ - 1));
    row_.has_rid = true;
    tuple = &row_;
    return RC::SUCCESS;
  }
  RC close() override { CHECK(active); active = false; ++closes; return close_error; }
private:
  Rows rows_;
  int table_;
  size_t cursor_ = 0;
  Tuple row_;
};

class RefValue : public Expression
{
public:
  RefValue(int column, AttrType type) : Expression(ExprType::COLUMN), ref_(0, column), type_(type) {}
  RC evaluate(const Tuple &tuple, Value &value) const override
  {
    return tuple.get(ref_, value) ? RC::SUCCESS : RC::SCHEMA_FIELD_NOT_EXIST;
  }
  AttrType result_type() const override { return type_; }
  std::string describe() const override { return "col" + std::to_string(ref_.column_index); }
private:
  ColumnRef ref_;
  AttrType type_;
};

class FailingValue : public Expression
{
public:
  FailingValue() : Expression(ExprType::COLUMN) {}
  RC evaluate(const Tuple &, Value &) const override { return RC::IOERR_READ; }
  std::string describe() const override { return "failing_value"; }
};

class Capture : public ResultWriter
{
public:
  Rows rows;
  SqlResult summary;
  RC write_header(const SqlResult &) override { rows.clear(); return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override { rows.push_back(tuple.values); return RC::SUCCESS; }
  RC write_result(const SqlResult &result) override { summary = result; return RC::SUCCESS; }
};

static RC run(PhysicalOperator &plan, Capture &writer, QueryEngine &engine)
{
  SqlResult input;
  input.is_query = true;
  return engine.run_plan(plan, input, writer);
}

static void test_filter_project()
{
  RefValue id(0, AttrType::INT), name(1, AttrType::CHARS);
  ComparisonExpression condition(std::make_unique<RefValue>(0, AttrType::INT), CompOp::GREAT_EQUAL,
                                 std::make_unique<ConstExpression>(Value(2)));
  auto source = std::make_unique<RowSource>(Rows{{Value(1), Value("a")}, {Value(2), Value("b")},
                                               {Value(2), Value("b")}, {Value(3), Value("c")}});
  auto *leaf = source.get();
  auto filter = std::make_unique<FilterPhysicalOperator>(std::vector<Expression *>{&condition});
  auto *predicate = filter.get();
  filter->add_child(std::move(source));
  ProjectPhysicalOperator project({&name, &id}); // Reverse columns and preserve duplicates.
  project.add_child(std::move(filter));
  QueryEngine engine(nullptr);
  engine.set_profile_enabled(true);
  for (int pass = 0; pass < 2; ++pass) {
    Capture writer;
    CHECK(run(project, writer, engine) == RC::SUCCESS);
    CHECK(writer.rows.size() == 3);
    CHECK(writer.rows[0][0].get_string() == "b" && writer.rows[1][0].get_string() == "b");
    CHECK(writer.rows[2][1].get_int32() == 3);
    CHECK(project.statistics().output_rows == 3 && project.input_rows() == 3);
    CHECK(predicate->input_rows() == 4 && predicate->statistics().output_rows == 3);
    CHECK(leaf->statistics().output_rows == 4 && leaf->statistics().next_calls == 5);
    CHECK(leaf->statistics().open_calls == 1 && leaf->statistics().close_calls == 1);
    CHECK(leaf->statistics().errors == 0); // EOF is not an error.
    CHECK(engine.last_profile().find("inclusive_us=") != std::string::npos);
  }
  CHECK(leaf->opens == 2 && leaf->closes == 2);
  engine.set_profile_enabled(false);
  Capture writer;
  CHECK(run(project, writer, engine) == RC::SUCCESS);
  CHECK(writer.rows.size() == 3 && engine.last_profile().empty());
  CHECK(project.statistics().next_calls == 0 && leaf->statistics().output_rows == 0);

  ProjectPhysicalOperator missing_child({&id});
  Tuple *tuple = nullptr;
  CHECK(missing_child.next(tuple) == RC::INTERNAL);
  RefValue missing(9, AttrType::INT);
  ProjectPhysicalOperator missing_column({&missing});
  missing_column.add_child(std::make_unique<RowSource>(Rows{{Value(1)}}));
  CHECK(run(missing_column, writer, engine) == RC::SCHEMA_FIELD_NOT_EXIST);

  for (int mode = 0; mode < 3; ++mode) {
    ConstExpression yes(Value(true)), no(Value(false)), wrong(Value(1));
    FilterPhysicalOperator f({mode == 0 ? &yes : mode == 1 ? &no : &wrong});
    f.add_child(std::make_unique<RowSource>(Rows{{Value(1)}, {Value(2)}}));
    CHECK(run(f, writer, engine) == (mode == 2 ? RC::SCHEMA_FIELD_TYPE_MISMATCH : RC::SUCCESS));
    CHECK(writer.rows.size() == (mode == 0 ? 2u : 0u));
  }
  ProjectPhysicalOperator empty({&id});
  empty.add_child(std::make_unique<RowSource>(Rows{}));
  CHECK(run(empty, writer, engine) == RC::SUCCESS && writer.rows.empty());

  ProjectPhysicalOperator rid_project({&id});
  rid_project.add_child(std::make_unique<RowSource>(Rows{{Value(7)}}));
  CHECK(rid_project.open() == RC::SUCCESS);
  CHECK(rid_project.next(tuple) == RC::SUCCESS);
  CHECK(tuple->has_rid && tuple->rid.page_num == 2 && tuple->rid.slot_num == 0);
  CHECK(rid_project.close() == RC::SUCCESS);
}

static void test_scalar()
{
  AggregateExpression count(AggFunc::COUNT, nullptr);
  AggregateExpression sum(AggFunc::SUM, std::make_unique<RefValue>(0, AttrType::INT));
  AggregateExpression avg(AggFunc::AVG, std::make_unique<RefValue>(0, AttrType::INT));
  AggregateExpression min(AggFunc::MIN, std::make_unique<RefValue>(0, AttrType::INT));
  AggregateExpression max(AggFunc::MAX, std::make_unique<RefValue>(0, AttrType::INT));
  ScalarGroupByOperator scalar({&count, &sum, &avg, &min, &max});
  auto source = std::make_unique<RowSource>(Rows{{Value(-2)}, {Value(4)}, {Value(4)}});
  auto *leaf = source.get();
  scalar.add_child(std::move(source));
  QueryEngine engine(nullptr);
  engine.set_profile_enabled(true);
  for (int pass = 0; pass < 2; ++pass) {
    Capture writer;
    CHECK(run(scalar, writer, engine) == RC::SUCCESS);
    CHECK(writer.rows.size() == 1 && writer.rows[0].size() == 5);
    const auto &r = writer.rows[0];
    CHECK(r[0].get_int64() == 3 && r[1].get_int64() == 6);
    CHECK(std::fabs(r[2].get_float() - 2.0f) < 0.00001f);
    CHECK(r[3].get_int32() == -2 && r[4].get_int32() == 4);
    CHECK(scalar.input_rows() == 3 && scalar.statistics().output_rows == 1);
    CHECK(leaf->statistics().output_rows == 3 && leaf->statistics().next_calls == 4);
    CHECK(scalar.statistics().open_ns >= leaf->statistics().next_ns);
    CHECK(!leaf->active && leaf->opens == leaf->closes);
    CHECK(scalar.close() == RC::SUCCESS); // No duplicate close after run_plan.
  }
  ScalarGroupByOperator empty({&count, &sum, &avg});
  empty.add_child(std::make_unique<RowSource>(Rows{}));
  Capture writer;
  CHECK(run(empty, writer, engine) == RC::SUCCESS);
  // This project has no NULL: document its zero-result simplification.
  CHECK(writer.rows.size() == 1 && writer.rows[0][0].get_int64() == 0);
  CHECK(writer.rows[0][1].get_int64() == 0 && writer.rows[0][2].get_float() == 0.0f);

  AggregateExpression count_column(AggFunc::COUNT, std::make_unique<RefValue>(0, AttrType::FLOAT));
  AggregateExpression real_sum(AggFunc::SUM, std::make_unique<RefValue>(0, AttrType::FLOAT));
  AggregateExpression real_avg(AggFunc::AVG, std::make_unique<RefValue>(0, AttrType::FLOAT));
  ScalarGroupByOperator reals({&count_column, &real_sum, &real_avg});
  reals.add_child(std::make_unique<RowSource>(Rows{{Value(1.5f)}, {Value(-0.5f)}}));
  CHECK(run(reals, writer, engine) == RC::SUCCESS);
  CHECK(writer.rows[0][0].get_int64() == 2);
  CHECK(std::fabs(writer.rows[0][1].get_float() - 1.0f) < 0.00001f);
  CHECK(std::fabs(writer.rows[0][2].get_float() - 0.5f) < 0.00001f);
}

static void test_integer_sum_limits()
{
  const int64_t high = std::numeric_limits<int64_t>::max();
  const int64_t low = std::numeric_limits<int64_t>::min();
  RefValue column(0, AttrType::BIGINT);
  auto tuple = [](int64_t n) { Tuple row; row.append(Value(n), ColumnRef(0,0)); return row; };
  const std::vector<std::pair<std::vector<int64_t>,int64_t>> valid{
      {{high},high}, {{low},low}, {{high-1,1},high}, {{low+1,-1},low},
      {{high,low},-1}, {{low,high},-1}, {{high,0},high}, {{low,0},low}};
  for (const auto &test : valid) {
    AggregateState sum; CHECK(sum.init(AggFunc::SUM,&column,"limits") == RC::SUCCESS);
    for (int64_t n : test.first) CHECK(sum.accumulate(tuple(n)) == RC::SUCCESS);
    Value result; CHECK(sum.result(result) == RC::SUCCESS);
    CHECK(result.type() == AttrType::BIGINT && result.get_int64() == test.second);
  }
  for (bool negative : {false,true}) {
    AggregateState sum; CHECK(sum.init(AggFunc::SUM,&column,"limits") == RC::SUCCESS);
    CHECK(sum.accumulate(tuple(negative ? low : high)) == RC::SUCCESS);
    CHECK(sum.accumulate(tuple(negative ? -1 : 1)) == RC::NUMERIC_OVERFLOW);
    Value result; CHECK(sum.result(result) == RC::SUCCESS && result.get_int64() == (negative ? low : high));
    CHECK(sum.accumulate(tuple(negative ? 1 : -1)) == RC::SUCCESS);
    CHECK(sum.result(result) == RC::SUCCESS && result.get_int64() == (negative ? low+1 : high-1));
  }
  RefValue integer(0, AttrType::INT);
  AggregateState promoted; CHECK(promoted.init(AggFunc::SUM,&integer,"int_sum") == RC::SUCCESS);
  Tuple max_int; max_int.append(Value(std::numeric_limits<int32_t>::max()),ColumnRef(0,0));
  CHECK(promoted.accumulate(max_int) == RC::SUCCESS && promoted.accumulate(max_int) == RC::SUCCESS);
  Value total; CHECK(promoted.result(total) == RC::SUCCESS && total.get_int64() == 4294967294LL);

  const std::vector<std::vector<int64_t>> overflow{{high,1},{low,-1},{1,high},{-1,low},{high,high},{low,low}};
  for (bool grouped : {false,true}) for (const auto &values : overflow) for (bool close_fails : {false,true}) {
    RefValue group(0,AttrType::INT);
    AggregateExpression sum(AggFunc::SUM,std::make_unique<RefValue>(1,AttrType::BIGINT));
    std::unique_ptr<PhysicalOperator> plan;
    if (grouped) plan = std::make_unique<HashGroupByOperator>(std::vector<Expression *>{&group,&sum},std::vector<Expression *>{&group});
    else plan = std::make_unique<ScalarGroupByOperator>(std::vector<Expression *>{&sum});
    Rows input;
    if (grouped) input.push_back({Value(0),Value(int64_t(0))}); // A prior valid group must not leak partial output.
    for (int64_t n : values) input.push_back({Value(1),Value(n)});
    auto source = std::make_unique<RowSource>(input); auto *leaf = source.get();
    if (close_fails) leaf->close_error = RC::IOERR_CLOSE;
    plan->add_child(std::move(source));
    QueryEngine engine(nullptr); engine.set_profile_enabled(true); Capture out;
    CHECK(run(*plan,out,engine) == RC::NUMERIC_OVERFLOW);
    CHECK(out.rows.empty() && out.summary.code == RC::NUMERIC_OVERFLOW);
    CHECK(!leaf->active && leaf->closes == 1 && leaf->statistics().close_calls == 1);
    CHECK(plan->statistics().errors == 1 && plan->close() == RC::SUCCESS && leaf->closes == 1);
    leaf->close_error = RC::SUCCESS;
    leaf->replace_rows(Rows{{Value(1),Value(high)},{Value(1),Value(low)}});
    CHECK(run(*plan,out,engine) == RC::SUCCESS && out.rows.size() == 1);
    CHECK(out.rows[0][grouped ? 1 : 0].get_int64() == -1);
    CHECK(!leaf->active && leaf->opens == 2 && leaf->closes == 2);
  }
}

static void test_group_keys()
{
  QueryEngine engine(nullptr);
  RefValue a(0, AttrType::CHARS), b(1, AttrType::CHARS);
  AggregateExpression count(AggFunc::COUNT, nullptr);
  HashGroupByOperator strings({&a, &b, &count}, {&a, &b});
  // Old delimiter encoding made these two distinct key pairs identical.
  strings.add_child(std::make_unique<RowSource>(Rows{{Value("a;4|b"), Value("c")},
                                                    {Value("a"), Value("b;4|c")},
                                                    {Value("a;4|b"), Value("c")}}));
  for (int pass = 0; pass < 2; ++pass) {
    Capture writer;
    CHECK(run(strings, writer, engine) == RC::SUCCESS);
    CHECK(writer.rows.size() == 2);
    CHECK(writer.rows[0][0].get_string() == "a;4|b" && writer.rows[0][2].get_int64() == 2);
    CHECK(writer.rows[1][0].get_string() == "a" && writer.rows[1][2].get_int64() == 1);
  }

  RefValue f(0, AttrType::FLOAT);
  HashGroupByOperator floats({&f, &count}, {&f});
  floats.add_child(std::make_unique<RowSource>(Rows{{Value(1.0000001f)}, {Value(1.0000002f)},
                                                   {Value(1.0000001f)}, {Value(-0.0f)}, {Value(0.0f)}}));
  Capture writer;
  CHECK(run(floats, writer, engine) == RC::SUCCESS);
  CHECK(writer.rows.size() == 3);
  CHECK(writer.rows[0][0].get_float() == 1.0000001f && writer.rows[0][1].get_int64() == 2);
  CHECK(writer.rows[1][0].get_float() == 1.0000002f && writer.rows[1][1].get_int64() == 1);
  CHECK(writer.rows[2][1].get_int64() == 2); // Signed zero must be one group.

  RefValue integer(0, AttrType::INT);
  HashGroupByOperator empty({&integer, &count}, {&integer});
  empty.add_child(std::make_unique<RowSource>(Rows{}));
  CHECK(run(empty, writer, engine) == RC::SUCCESS && writer.rows.empty());
}

static void test_aggregate_failure_cleanup()
{
  for (int grouped = 0; grouped < 2; ++grouped) {
    for (int mode = 0; mode < 4; ++mode) {
      RefValue group(0, AttrType::INT);
      AggregateExpression count(AggFunc::COUNT, nullptr);
      AggregateExpression failure(AggFunc::SUM, std::make_unique<FailingValue>());
      auto *aggregate = mode == 1 ? &failure : &count;
      std::unique_ptr<PhysicalOperator> plan;
      if (grouped) plan = std::make_unique<HashGroupByOperator>(std::vector<Expression *>{&group, aggregate},
                                                               std::vector<Expression *>{&group});
      else plan = std::make_unique<ScalarGroupByOperator>(std::vector<Expression *>{aggregate});
      auto source = std::make_unique<RowSource>(Rows{{Value(1)}});
      auto *leaf = source.get();
      if (mode == 0) leaf->read_error = RC::IOERR_READ;
      if (mode == 2) { leaf->read_error = RC::IOERR_READ; leaf->close_error = RC::IOERR_CLOSE; }
      if (mode == 3) leaf->close_error = RC::IOERR_CLOSE;
      plan->add_child(std::move(source));
      QueryEngine engine(nullptr);
      engine.set_profile_enabled(true);
      Capture writer;
      CHECK(run(*plan, writer, engine) == (mode == 3 ? RC::IOERR_CLOSE : RC::IOERR_READ));
      CHECK(!leaf->active && leaf->closes == 1);
      CHECK(leaf->statistics().close_calls == 1);
      CHECK(plan->close() == RC::SUCCESS && leaf->closes == 1);
      leaf->read_error = leaf->close_error = RC::SUCCESS;
      // Reuse after a read failure; the intentionally failing expression cannot succeed.
      if (mode != 1) {
        CHECK(run(*plan, writer, engine) == RC::SUCCESS);
        CHECK(writer.rows.size() == 1 && leaf->opens == 2 && leaf->closes == 2);
      }
    }
  }
}

static void test_join_statistics()
{
  NestedLoopJoinOperator join(nullptr);
  auto left = std::make_unique<RowSource>(Rows{{Value(1)}, {Value(2)}, {Value(3)}}, 0);
  auto right = std::make_unique<RowSource>(Rows{{Value(7)}, {Value(8)}}, 1);
  auto *r = right.get();
  join.add_child(std::move(left)); join.add_child(std::move(right));
  QueryEngine engine(nullptr);
  engine.set_profile_enabled(true);
  for (int pass = 0; pass < 2; ++pass) {
    Capture writer;
    CHECK(run(join, writer, engine) == RC::SUCCESS && writer.rows.size() == 6);
    CHECK(join.statistics().candidate_pairs == 6 && join.statistics().output_rows == 6);
    CHECK(join.input_rows() == 9); // 3 left rows + 2 right rows delivered 3 times.
    CHECK(r->statistics().open_calls == 3 && r->statistics().close_calls == 3);
    CHECK(r->statistics().next_calls == 9 && r->statistics().output_rows == 6);
  }
}

static void test_real_sql()
{
  const std::string dir = "./build/test_execution_operators";
  CHECK(path_mkdir_recursive(dir) == RC::SUCCESS);
  Db *raw = nullptr;
  RC rc = Db::open(dir, "operators", &raw);
  if (rc == RC::SCHEMA_DB_NOT_EXIST || rc == RC::FILE_NOT_EXIST) rc = Db::create(dir, "operators", &raw);
  CHECK(rc == RC::SUCCESS);
  std::unique_ptr<Db> db(raw);
  QueryEngine engine(db.get());
  Capture writer;
  auto sql = [&](const std::string &text) {
    writer.rows.clear();
    SqlResult result;
    CHECK(engine.execute(text, writer, result) == RC::SUCCESS);
  };
  sql("drop table if exists profile_left"); sql("drop table if exists profile_right");
  sql("create table profile_left (id int)"); sql("create table profile_right (id int)");
  sql("insert into profile_left values (1), (1), (2)");
  sql("insert into profile_right values (1), (2)");
  const std::string query = "select l.id, r.id from profile_left l, profile_right r where l.id=r.id";
  sql(query);
  CHECK(writer.rows.size() == 3 && engine.last_profile().empty());
  auto join_rows = [](const Rows &rows) {
    std::vector<std::pair<int32_t, int32_t>> values;
    for (const auto &row : rows) {
      CHECK(row.size() == 2 && row[0].type() == AttrType::INT && row[1].type() == AttrType::INT);
      values.emplace_back(row[0].get_int32(), row[1].get_int32());
    }
    std::sort(values.begin(), values.end());
    return values;
  };
  const std::vector<std::pair<int32_t, int32_t>> expected{{1, 1}, {1, 1}, {2, 2}};
  CHECK(join_rows(writer.rows) == expected); // Preserve duplicate multiplicity, ignore unspecified order.
  engine.set_profile_enabled(true);
  for (const std::string &text : {query, std::string(
      "select l.id, r.id from profile_right r, profile_left l where r.id=l.id")}) {
    sql(text);
    CHECK(join_rows(writer.rows) == expected);
    const std::string &profile = engine.last_profile();
    CHECK(profile.find("profile_left") != std::string::npos);
    CHECK(profile.find("profile_right") != std::string::npos);
    // SQL plans may reorder inputs or select a different join algorithm. Exact NLJ
    // candidates/right reopen counts remain covered in test_join_statistics().
    std::istringstream lines(profile);
    std::string line;
    int operators = 0;
    while (std::getline(lines, line)) {
      if (line.find(" in_rows=") == std::string::npos) continue;
      auto metric = [&](const std::string &name) -> int64_t {
        const std::string prefix = " " + name + "=";
        const size_t pos = line.find(prefix);
        CHECK(pos != std::string::npos);
        return std::stoll(line.substr(pos + prefix.size()));
      };
      CHECK(metric("errors") == 0);
      CHECK(metric("opens") == metric("closes"));
      CHECK(metric("nexts") >= metric("out_rows"));
      if (operators++ == 0) {
        CHECK(metric("out_rows") == 3 && metric("opens") == 1 && metric("nexts") == 4);
      }
    }
    CHECK(operators >= 3); // Root and both input scans must actually be profiled.
  }
  sql("select count(*), sum(id) from profile_left");
  CHECK(writer.rows.size() == 1 && writer.rows[0][0].get_int64() == 3 && writer.rows[0][1].get_int64() == 4);
  CHECK(engine.last_profile().find("in_rows=3 out_rows=1") != std::string::npos);
  SqlResult error;
  CHECK(engine.execute("select * from absent_table", writer, error) == RC::SCHEMA_TABLE_NOT_EXIST);
  CHECK(engine.last_profile().empty()); // No stale previous profile on compilation failure.

  sql("drop table if exists profile_sum_limits");
  sql("create table profile_sum_limits (g int, v bigint)");
  for (bool negative : {false,true}) {
    sql("delete from profile_sum_limits");
    sql(negative ? "insert into profile_sum_limits values (0,0),(1,-9223372036854775807),(1,-1),(1,-1)"
                 : "insert into profile_sum_limits values (0,0),(1,9223372036854775807),(1,1)");
    for (const char *text : {"select sum(v) from profile_sum_limits", "select g,sum(v) from profile_sum_limits group by g"}) {
      writer.rows.clear(); SqlResult result;
      CHECK(engine.execute(text,writer,result) == RC::NUMERIC_OVERFLOW);
      CHECK(writer.summary.code == RC::NUMERIC_OVERFLOW && writer.rows.empty());
      CHECK(engine.last_profile().find("errors=1") != std::string::npos);
    }
  }
  sql("delete from profile_sum_limits"); // Must not deadlock: failed aggregate scans have been closed.
  sql("insert into profile_sum_limits values (1,9223372036854775806),(1,1),(2,-9223372036854775807),(2,-1)");
  sql("select g,sum(v) from profile_sum_limits group by g");
  CHECK(writer.rows.size() == 2);
  for (const auto &row : writer.rows) {
    CHECK(row[0].get_int32() == 1 || row[0].get_int32() == 2);
    CHECK(row[1].type() == AttrType::BIGINT);
    CHECK(row[1].get_int64() == (row[0].get_int32() == 1 ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min()));
  }
  sql("select sum(v) from profile_sum_limits");
  CHECK(writer.rows.size() == 1 && writer.rows[0][0].get_int64() == -1);
  CHECK(db->close() == RC::SUCCESS);
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::string(argv[1]) == "--group-keys-only") {
    test_group_keys();
  } else {
    test_filter_project();
    test_scalar();
    test_integer_sum_limits();
    test_group_keys();
    test_aggregate_failure_cleanup();
    test_join_statistics();
    test_real_sql();
  }
  std::puts("execution operators v3-v4: all tests passed");
  return 0;
}
