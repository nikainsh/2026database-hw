// Execution v1: fault-injection tests for plan cleanup and error propagation.
// CHECK remains active in Release builds (unlike assert with NDEBUG).
#include <cstdio>
#include <cstdlib>

#include "sql/query_engine.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
  std::exit(1); } } while (false)

class ProbeOperator : public PhysicalOperator
{
public:
  RC open_rc = RC::SUCCESS, next_rc = RC::SUCCESS, close_rc = RC::SUCCESS;
  int opens = 0, nexts = 0, closes = 0;
  bool active = false;
  Tuple row;

  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "probe"; }
  RC open() override
  {
    ++opens;
    nexts = 0;
    active = OB_SUCC(open_rc);
    return open_rc;
  }
  RC next(Tuple *&tuple) override
  {
    CHECK(active);
    ++nexts;
    if (OB_FAIL(next_rc)) return next_rc;
    if (nexts > 1) return RC::RECORD_EOF;
    tuple = &row;
    return RC::SUCCESS;
  }
  RC close() override
  {
    CHECK(active);
    active = false;
    ++closes;
    return close_rc;
  }
};

class ProbeWriter : public ResultWriter
{
public:
  RC header_rc = RC::SUCCESS, row_rc = RC::SUCCESS, result_rc = RC::SUCCESS;
  int headers = 0, rows = 0, results = 0;
  SqlResult summary;
  RC write_header(const SqlResult &) override { ++headers; return header_rc; }
  RC write_row(const SqlResult &, const Tuple &) override { ++rows; return row_rc; }
  RC write_result(const SqlResult &result) override
  {
    ++results;
    summary = result;
    return result_rc;
  }
};

static void test_case(int scenario)
{
  QueryEngine engine(nullptr); // A prepared plan needs no database or SQL parser.
  ProbeOperator plan;
  ProbeWriter writer;
  SqlResult input;
  input.is_query = true;
  RC expected = RC::SUCCESS;
  switch (scenario) {
    case 1: writer.header_rc = expected = RC::IOERR_WRITE; break;
    case 2: writer.row_rc = expected = RC::IOERR_WRITE; break;
    case 3: plan.next_rc = expected = RC::IOERR_READ; break;
    case 4: plan.close_rc = expected = RC::IOERR_CLOSE; break;
    case 5: writer.result_rc = expected = RC::IOERR_WRITE; break;
    case 6:
      writer.header_rc = expected = RC::IOERR_WRITE;
      plan.close_rc = RC::IOERR_CLOSE;
      writer.result_rc = RC::IOERR_ACCESS;
      break;
    case 7:
      plan.next_rc = expected = RC::IOERR_READ;
      plan.close_rc = RC::IOERR_CLOSE;
      writer.result_rc = RC::IOERR_WRITE;
      break;
    case 8: input.is_query = false; break;
    case 9: plan.open_rc = expected = RC::NOMEM; break;
  }
  CHECK(engine.run_plan(plan, input, writer) == expected);
  CHECK(plan.opens == 1);
  CHECK(plan.closes == (scenario == 9 ? 0 : 1));
  CHECK(!plan.active);
  CHECK(writer.headers == (scenario == 8 || scenario == 9 ? 0 : 1));
  CHECK(writer.results == 1);
  // A writer cannot receive its own future failure in the supplied summary.
  CHECK(writer.summary.code == (scenario == 5 ? RC::SUCCESS : expected));
  if (scenario == 1 || scenario == 6 || scenario == 9) {
    CHECK(plan.nexts == 0);
    CHECK(writer.rows == 0);
    CHECK(writer.summary.rows == 0);
  }
  if (scenario == 0) {
    CHECK(plan.nexts == 2);
    CHECK(writer.rows == 1);
    CHECK(writer.summary.rows == 1);
    // The same plan can be opened again after a completed execution.
    ProbeWriter second;
    CHECK(engine.run_plan(plan, input, second) == RC::SUCCESS);
    CHECK(plan.opens == 2 && plan.closes == 2);
    CHECK(second.rows == 1 && second.summary.rows == 1);
  }
}

int main()
{
  for (int scenario = 0; scenario < 10; ++scenario) test_case(scenario);
  std::puts("execution lifecycle: 10 scenarios passed");
  return 0;
}
