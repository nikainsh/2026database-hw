// v2: exercise the real join with controlled leaf inputs, without disk fixtures.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "sql/operator/relation_operators.h"
#include "sql/operator/scan_operators.h"
#include "sql/query_engine.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
  std::exit(1); } } while (false)

// Reuses one tuple, like a scan. Rejects a close before open or a duplicate close.
class RowsOperator : public PhysicalOperator
{
public:
  RowsOperator(std::vector<int> values, int table) : values_(std::move(values)), table_(table) {}
  RC open_rc = RC::SUCCESS, read_rc = RC::SUCCESS, close_rc = RC::SUCCESS;
  int opens = 0, closes = 0, nexts = 0, invalid_closes = 0;
  bool active = false;
  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "rows"; }
  RC open() override
  {
    CHECK(!active);
    ++opens;
    cursor_ = 0;
    active = OB_SUCC(open_rc);
    return open_rc;
  }
  RC next(Tuple *&tuple) override
  {
    CHECK(active);
    ++nexts;
    if (OB_FAIL(read_rc)) return read_rc;
    if (cursor_ == values_.size()) return RC::RECORD_EOF;
    row_.clear();
    row_.append(Value(values_[cursor_++]), ColumnRef(table_, 0));
    tuple = &row_;
    return RC::SUCCESS;
  }
  RC close() override
  {
    if (!active) { ++invalid_closes; return RC::INTERNAL; }
    active = false;
    ++closes;
    return close_rc;
  }
private:
  std::vector<int> values_;
  int table_;
  size_t cursor_ = 0;
  Tuple row_;
};

// Only replaces name binding; comparison still uses the production expression.
class RefExpression : public Expression
{
public:
  explicit RefExpression(int table) : Expression(ExprType::COLUMN), ref_(table, 0) {}
  RC evaluate(const Tuple &tuple, Value &value) const override
  {
    return tuple.get(ref_, value) ? RC::SUCCESS : RC::SCHEMA_FIELD_NOT_EXIST;
  }
  std::string describe() const override { return "ref"; }
private:
  ColumnRef ref_;
};

using Pair = std::pair<int, int>;
class PairWriter : public ResultWriter
{
public:
  std::vector<Pair> rows;
  RC header_rc = RC::SUCCESS, row_rc = RC::SUCCESS;
  SqlResult summary;
  RC write_header(const SqlResult &) override { return header_rc; }
  RC write_row(const SqlResult &, const Tuple &tuple) override
  {
    Value left, right;
    CHECK(tuple.get(ColumnRef(0, 0), left));
    CHECK(tuple.get(ColumnRef(1, 0), right));
    rows.emplace_back(left.get_int32(), right.get_int32());
    return row_rc;
  }
  RC write_result(const SqlResult &result) override { summary = result; return RC::SUCCESS; }
};

struct JoinFixture
{
  NestedLoopJoinOperator join;
  RowsOperator *left, *right;
  JoinFixture(std::vector<int> l, std::vector<int> r, Expression *condition = nullptr)
      : join(condition == nullptr ? std::vector<Expression *>() : std::vector<Expression *>{condition})
  {
    auto a = std::make_unique<RowsOperator>(std::move(l), 0);
    auto b = std::make_unique<RowsOperator>(std::move(r), 1);
    left = a.get(); right = b.get();
    join.add_child(std::move(a)); join.add_child(std::move(b));
  }
  void balanced() const
  {
    CHECK(!left->active && !right->active);
    CHECK(left->invalid_closes == 0 && right->invalid_closes == 0);
  }
};

static RC run(NestedLoopJoinOperator &join, PairWriter &writer)
{
  QueryEngine engine(nullptr);
  SqlResult input;
  input.is_query = true;
  return engine.run_plan(join, input, writer);
}

static void test_empty_inputs()
{
  for (int side = 0; side < 2; ++side) {
    JoinFixture f(side == 0 ? std::vector<int>{} : std::vector<int>{1, 2},
                  side == 0 ? std::vector<int>{3} : std::vector<int>{});
    PairWriter writer;
    CHECK(run(f.join, writer) == RC::SUCCESS);
    CHECK(writer.rows.empty() && writer.summary.rows == 0);
    CHECK(f.left->opens == 1 && f.left->closes == 1);
    CHECK(f.right->opens == (side == 0 ? 0 : 2));
    CHECK(f.right->closes == f.right->opens);
    f.balanced();
  }
}

static void test_results_and_reopen()
{
  // Left tuple must survive right-side reuse; duplicate values must not disappear.
  JoinFixture cross({1, 2}, {7, 7, 8});
  const std::vector<Pair> expected{{1, 7}, {1, 7}, {1, 8}, {2, 7}, {2, 7}, {2, 8}};
  for (int pass = 0; pass < 2; ++pass) {
    PairWriter writer;
    CHECK(run(cross.join, writer) == RC::SUCCESS);
    CHECK(writer.rows == expected);
    CHECK(writer.summary.rows == 6);
    cross.balanced();
  }
  CHECK(cross.right->opens == 4 && cross.right->closes == 4);

  ComparisonExpression equals(std::make_unique<RefExpression>(0), CompOp::EQUAL_TO,
                              std::make_unique<RefExpression>(1));
  JoinFixture equi({1, 1, 2}, {1, 1, 3}, &equals);
  PairWriter writer;
  CHECK(run(equi.join, writer) == RC::SUCCESS);
  CHECK(writer.rows == std::vector<Pair>(4, Pair(1, 1)));
  equi.balanced();
}

static void test_close_and_early_stop()
{
  JoinFixture f({1}, {2, 3});
  CHECK(f.join.close() == RC::SUCCESS); // Never opened.
  CHECK(f.join.open() == RC::SUCCESS);
  CHECK(f.join.open() == RC::INVALID_ARGUMENT); // Cannot reset active children.
  Tuple *row = nullptr;
  CHECK(f.join.next(row) == RC::SUCCESS); // Stop before exhausting the right input.
  CHECK(f.join.close() == RC::SUCCESS);
  CHECK(f.join.close() == RC::SUCCESS); // No duplicate child close.
  CHECK(f.join.next(row) == RC::INTERNAL);
  CHECK(f.left->closes == 1 && f.right->closes == 1);
  f.balanced();
  PairWriter writer;
  CHECK(run(f.join, writer) == RC::SUCCESS);
  CHECK(writer.rows.size() == 2);
  f.balanced();
}

static void test_failures()
{
  for (int failure = 0; failure < 8; ++failure) {
    JoinFixture f({1, 2}, {3});
    PairWriter writer;
    RC expected;
    switch (failure) {
      case 0: writer.header_rc = expected = RC::IOERR_WRITE; break;
      case 1: f.left->open_rc = expected = RC::NOMEM; break;
      case 2: f.right->open_rc = expected = RC::NOMEM; break;
      case 3: f.right->read_rc = expected = RC::IOERR_READ; break;
      case 4: f.right->close_rc = expected = RC::IOERR_CLOSE; break;
      case 5:
        f.right->close_rc = expected = RC::IOERR_CLOSE;
        f.left->close_rc = RC::IOERR_ACCESS;
        break;
      case 6: f.left->read_rc = expected = RC::IOERR_READ; break;
      default: writer.row_rc = expected = RC::IOERR_WRITE; break;
    }
    CHECK(run(f.join, writer) == expected);
    CHECK(writer.summary.code == expected);
    f.balanced();
    CHECK(f.left->closes == (failure == 1 ? 0 : 1));
    CHECK(f.right->closes == (failure < 3 || failure == 6 ? 0 : 1));
    if (failure == 4 || failure == 5) CHECK(writer.rows.size() == 1);
  }
}

static void test_two_close_errors()
{
  JoinFixture f({1}, {2});
  CHECK(f.join.open() == RC::SUCCESS);
  Tuple *row = nullptr;
  CHECK(f.join.next(row) == RC::SUCCESS);
  f.right->close_rc = RC::IOERR_CLOSE;
  f.left->close_rc = RC::IOERR_ACCESS;
  CHECK(f.join.close() == RC::IOERR_CLOSE);
  CHECK(f.left->closes == 1 && f.right->closes == 1);
  f.balanced();
}

static void test_unopened_index_input()
{
  NestedLoopJoinOperator join(std::vector<Expression *>{});
  join.add_child(std::make_unique<RowsOperator>(std::vector<int>{}, 0));
  // Empty left means this real IndexScanOperator must never be opened or closed.
  // Null dependencies deliberately detect any attempt to touch the unused side.
  join.add_child(std::make_unique<IndexScanOperator>(nullptr, nullptr,
                 std::vector<Expression *>{}, IndexScanRange{}, 1));
  PairWriter writer;
  CHECK(run(join, writer) == RC::SUCCESS);
  CHECK(writer.rows.empty());
}

static void test_nested_join()
{
  NestedLoopJoinOperator outer(std::vector<Expression *>{});
  // Use a separate inner tree owned by the outer join, exercising recursive close.
  auto nested = std::make_unique<NestedLoopJoinOperator>(std::vector<Expression *>{});
  auto a = std::make_unique<RowsOperator>(std::vector<int>{1, 2}, 0);
  auto b = std::make_unique<RowsOperator>(std::vector<int>{3}, 1);
  auto c = std::make_unique<RowsOperator>(std::vector<int>{4, 5}, 2);
  RowsOperator *ap = a.get(), *bp = b.get(), *cp = c.get();
  nested->add_child(std::move(a)); nested->add_child(std::move(b));
  outer.add_child(std::move(nested)); outer.add_child(std::move(c));
  for (int pass = 0; pass < 2; ++pass) {
    CHECK(outer.open() == RC::SUCCESS);
    Tuple *row = nullptr;
    int count = 0;
    RC rc;
    while ((rc = outer.next(row)) == RC::SUCCESS) {
      CHECK(row->column_count() == 3);
      Value x, y, z;
      CHECK(row->get(ColumnRef(0, 0), x) && row->get(ColumnRef(1, 0), y));
      CHECK(row->get(ColumnRef(2, 0), z));
      CHECK(x.get_int32() == 1 + count / 2 && y.get_int32() == 3);
      CHECK(z.get_int32() == 4 + count % 2);
      ++count;
    }
    CHECK(rc == RC::RECORD_EOF && count == 4);
    CHECK(outer.close() == RC::SUCCESS);
    CHECK(!ap->active && !bp->active && !cp->active);
    CHECK(ap->opens == ap->closes && bp->opens == bp->closes && cp->opens == cp->closes);
    CHECK(ap->invalid_closes == 0 && bp->invalid_closes == 0 && cp->invalid_closes == 0);
  }
}

int main()
{
  test_empty_inputs();
  test_results_and_reopen();
  test_close_and_early_stop();
  test_failures();
  test_two_close_errors();
  test_unopened_index_input();
  test_nested_join();
  std::puts("nested loop join v2: all tests passed");
  return 0;
}
