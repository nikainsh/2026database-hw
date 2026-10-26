// Reproducible operator workloads. Default: small correctness matrix.
// --bench: repeated timings with profiling disabled; CSV + raw samples.
// No production code, SQL optimizer policy, or storage format is changed here.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sql/operator/hash_join_operator.h"
#include "sql/operator/relation_operators.h"
#include "sql/query_engine.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using Rows = std::vector<std::vector<Value>>;
using Pair = std::pair<int, int>;
using Clock = std::chrono::steady_clock;

struct Workload {
  std::string name;
  Rows left, right;
  int payload_bytes = 16;
};

static std::vector<Value> row(int first, int second, int tag, int width) {
  return {Value(first), Value(second), Value(std::string(static_cast<size_t>(width), static_cast<char>('a' + tag % 20))), Value(tag)};
}

static std::vector<Workload> workloads(int n) {
  std::vector<Workload> cases;
  for (const std::string name : {"tiny", "unique", "duplicate_groups", "no_match", "same_prefix_no_match",
                                "hot_prefix", "hot_full_key", "wide_payload", "empty_left", "empty_right"}) {
    Workload w; w.name = name;
    const int count = name == "tiny" ? 8 : (name == "wide_payload" ? std::min(n, 300) : n);
    w.payload_bytes = name == "wide_payload" ? 2048 : 16;
    for (int i = 0; i < count; ++i) {
      int a = i, b = i, ra = i, rb = i;
      if (name == "duplicate_groups") {
        const int groups = std::min(20,std::max(2,count/10));
        a = ra = i % groups; b = rb = (i / groups) % 5;
      }
      if (name == "no_match") ra += count;
      if (name == "same_prefix_no_match") { a = ra = 1; rb += count; }
      if (name == "hot_prefix") a = ra = 1;
      if (name == "hot_full_key" && i < count * 4 / 5) a = b = ra = rb = 0;
      if (name != "empty_left") w.left.push_back(row(a, b, i, w.payload_bytes));
      if (name != "empty_right") w.right.push_back(row(ra, rb, i, w.payload_bytes));
    }
    cases.push_back(std::move(w));
  }
  return cases;
}

class Source : public PhysicalOperator {
public:
  Source(const Rows &rows, int table) : rows_(rows), table_(table) {}
  bool active = false;
  int64_t opens = 0, closes = 0;
  PhysicalOperatorKind kind() const override { return PhysicalOperatorKind::STRING_LIST; }
  std::string describe() const override { return "workload_source"; }
  RC open() override { CHECK(!active); active = true; cursor_ = 0; ++opens; return RC::SUCCESS; }
  RC next(Tuple *&tuple) override {
    CHECK(active);
    if (cursor_ == rows_.size()) return RC::RECORD_EOF;
    row_.clear();
    const auto &values = rows_[cursor_];
    for (size_t i = 0; i < values.size(); ++i) row_.append(values[i], ColumnRef(table_, static_cast<int>(i)));
    row_.rid = RID(1, static_cast<SlotNum>(cursor_++)); row_.has_rid = true;
    tuple = &row_; return RC::SUCCESS;
  }
  RC close() override { CHECK(active); active = false; ++closes; return RC::SUCCESS; }
private:
  const Rows &rows_;
  int table_;
  size_t cursor_ = 0;
  Tuple row_;
};

class BothEqual : public Expression {
public:
  BothEqual() : Expression(ExprType::COMPARISON) {}
  RC evaluate(const Tuple &tuple, Value &out) const override {
    for (int col = 0; col < 2; ++col) {
      Value a, b;
      if (!tuple.get(ColumnRef(0,col), a) || !tuple.get(ColumnRef(1,col), b)) return RC::SCHEMA_FIELD_NOT_EXIST;
      int cmp = 0; RC rc = a.compare_to(b, cmp);
      if (OB_FAIL(rc)) return rc;
      if (cmp) { out = Value(false); return RC::SUCCESS; }
    }
    out = Value(true); return RC::SUCCESS;
  }
  std::string describe() const override { return "left.k1=right.k1 AND left.k2=right.k2"; }
};

enum Algorithm { NESTED = 0, SINGLE = 1, COMPOSITE = 2 };
static const char *names[] = {"nested_loop", "single_hash_residual", "composite_hash"};

struct Plan {
  // Predicate must outlive the root which borrows its pointer.
  std::unique_ptr<BothEqual> predicate;
  std::unique_ptr<PhysicalOperator> root;
  Source *left = nullptr, *right = nullptr;
};

static Plan make_plan(const Workload &w, int algorithm) {
  Plan plan;
  plan.predicate = std::make_unique<BothEqual>();
  if (algorithm == NESTED) plan.root = std::make_unique<NestedLoopJoinOperator>(plan.predicate.get());
  else if (algorithm == SINGLE) plan.root = std::make_unique<HashJoinOperator>(
      ColumnRef(0,0), ColumnRef(1,0), AttrType::INT, plan.predicate.get());
  else plan.root = std::make_unique<HashJoinOperator>(std::vector<HashJoinKey>{
      {{0,0},{1,0},AttrType::INT}, {{0,1},{1,1},AttrType::INT}});
  auto l = std::make_unique<Source>(w.left, 0), r = std::make_unique<Source>(w.right, 1);
  plan.left = l.get(); plan.right = r.get();
  plan.root->add_child(std::move(l)); plan.root->add_child(std::move(r));
  return plan;
}

class CountSink : public ResultWriter {
public:
  int64_t count = 0;
  RC write_header(const SqlResult &) override { count = 0; return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &) override { ++count; return RC::SUCCESS; }
  RC write_result(const SqlResult &) override { return RC::SUCCESS; }
};

// Verify every output value and its source reference, then compare sorted row-ID
// pairs. This is a lossless multiset check because every input has a unique tag.
// Unlike a checksum, it detects missing/extra/duplicate combinations exactly.
class VerifySink : public CountSink {
public:
  explicit VerifySink(const Workload &w) : w_(w) {}
  std::vector<Pair> pairs;
  RC write_row(const SqlResult &result, const Tuple &tuple) override {
    CHECK(tuple.values.size() == 8 && tuple.refs.size() == 8);
    CHECK(tuple.values[3].type() == AttrType::INT && tuple.values[7].type() == AttrType::INT);
    const int l = tuple.values[3].get_int32(), r = tuple.values[7].get_int32();
    CHECK(l >= 0 && static_cast<size_t>(l) < w_.left.size() && r >= 0 && static_cast<size_t>(r) < w_.right.size());
    CHECK(tuple.has_rid && tuple.rid == RID(1,l));
    for (int side = 0; side < 2; ++side) for (int col = 0; col < 4; ++col) {
      const auto &expected = (side ? w_.right[r] : w_.left[l])[col];
      const auto &actual = tuple.values[side * 4 + col];
      int cmp = 0;
      CHECK(tuple.refs[side * 4 + col] == ColumnRef(side,col));
      CHECK(actual.type() == expected.type() && actual.compare_to(expected,cmp) == RC::SUCCESS && cmp == 0);
    }
    pairs.emplace_back(l,r);
    return CountSink::write_row(result,tuple);
  }
private:
  const Workload &w_;
};

static void execute(Plan &plan, ResultWriter &sink, bool profile) {
  QueryEngine engine(nullptr); engine.set_profile_enabled(profile);
  SqlResult result; result.is_query = true;
  CHECK(engine.run_plan(*plan.root,result,sink) == RC::SUCCESS);
  CHECK(!plan.left->active && !plan.right->active);
  CHECK(plan.left->opens == plan.left->closes && plan.right->opens == plan.right->closes);
}

struct Reference {
  std::vector<Pair> pairs;
  int64_t prefix_candidates = 0;
};

static Reference reference(const Workload &w) {
  Reference result;
  for (size_t l = 0; l < w.left.size(); ++l) for (size_t r = 0; r < w.right.size(); ++r) {
    if (w.left[l][0].get_int32() != w.right[r][0].get_int32()) continue;
    ++result.prefix_candidates;
    if (w.left[l][1].get_int32() == w.right[r][1].get_int32()) result.pairs.emplace_back(static_cast<int>(l),static_cast<int>(r));
  }
  return result;
}

struct Metrics {
  int64_t candidates = 0, build_rows = 0, probe_rows = 0, right_opens = 0;
  size_t bytes = 0;
};

static Metrics verify(const Workload &w, int algorithm, const Reference &expected) {
  auto plan = make_plan(w,algorithm); VerifySink sink(w);
  execute(plan,sink,true);
  std::sort(sink.pairs.begin(),sink.pairs.end());
  CHECK(sink.pairs == expected.pairs && sink.count == static_cast<int64_t>(expected.pairs.size()));
  const auto &s = plan.root->statistics();
  const int64_t candidates = algorithm == NESTED ? static_cast<int64_t>(w.left.size() * w.right.size()) :
      (algorithm == SINGLE ? expected.prefix_candidates : static_cast<int64_t>(expected.pairs.size()));
  CHECK(s.candidate_pairs == candidates && s.output_rows == sink.count && s.errors == 0);
  CHECK(s.open_calls == 1 && s.close_calls == 1 && s.next_calls == sink.count + 1);
  const int64_t right_opens = algorithm == NESTED ? static_cast<int64_t>(w.left.size()) : (w.left.empty() ? 0 : 1);
  CHECK(plan.right->statistics().open_calls == right_opens);
  if (algorithm != NESTED) {
    CHECK(s.build_rows == static_cast<int64_t>(w.left.size()));
    CHECK(s.probe_rows == static_cast<int64_t>(w.left.empty() ? 0 : w.right.size()));
    CHECK(w.left.empty() ? s.peak_build_bytes == 0 : s.peak_build_bytes > 0);
  }
  return {s.candidate_pairs,s.build_rows,s.probe_rows,right_opens,s.peak_build_bytes};
}

static double timed(const Workload &w, int algorithm, int64_t expected_rows) {
  auto plan = make_plan(w,algorithm); CountSink sink;
  // No input generation, plan construction, full validation, profiling clocks,
  // sorting, CSV writing or terminal rendering in the timed interval.
  const auto start = Clock::now();
  execute(plan,sink,false);
  const auto end = Clock::now();
  CHECK(sink.count == expected_rows);
  return std::chrono::duration<double,std::micro>(end - start).count();
}

static void benchmark(int rows, int repeats, const std::string &csv_path) {
  std::ofstream csv(csv_path), raw(csv_path + ".samples.csv");
  CHECK(csv.good() && raw.good());
  csv << "scenario,left_rows,right_rows,payload_bytes,output_rows,algorithm,candidate_pairs,build_rows,probe_rows,right_opens,peak_build_bytes_est,median_execute_us,min_execute_us,max_execute_us,repeats\n";
  raw << "scenario,algorithm,repeat,execute_us\n";
  csv.setf(std::ios::fixed); csv.precision(3); raw.setf(std::ios::fixed); raw.precision(3);
  std::puts("All three plans use identical two-key equality semantics. Exact output checks precede timings.");
  std::puts("Timings: one warm-up, rotating order, profiling OFF, count-only sink; microseconds, min/median/max in CSV.");
  std::puts("In-memory operator experiment: excludes SQL planning, disk I/O and output rendering. Memory is estimated build storage, not RSS.");
  int scenario = 0;
  for (const auto &w : workloads(rows)) {
    const auto expected = reference(w);
    std::array<Metrics,3> metrics;
    std::array<std::vector<double>,3> times;
    for (int a = 0; a < 3; ++a) { metrics[a] = verify(w,a,expected); timed(w,a,expected.pairs.size()); }
    for (int repeat = 0; repeat < repeats; ++repeat) for (int offset = 0; offset < 3; ++offset) {
      const int a = (repeat + offset + scenario) % 3;
      const double us = timed(w,a,expected.pairs.size());
      times[a].push_back(us);
      raw << w.name << ',' << names[a] << ',' << repeat + 1 << ',' << us << '\n';
    }
    std::array<double,3> medians;
    for (int a = 0; a < 3; ++a) {
      auto sorted = times[a]; std::sort(sorted.begin(),sorted.end());
      const double median = sorted.size() % 2 ? sorted[sorted.size()/2] : (sorted[sorted.size()/2-1] + sorted[sorted.size()/2]) / 2;
      medians[a] = median;
      const auto &m = metrics[a];
      csv << w.name << ',' << w.left.size() << ',' << w.right.size() << ',' << w.payload_bytes << ',' << expected.pairs.size()
          << ',' << names[a] << ',' << m.candidates << ',' << m.build_rows << ',' << m.probe_rows << ',' << m.right_opens
          << ',' << m.bytes << ',' << median << ',' << sorted.front() << ',' << sorted.back() << ',' << repeats << '\n';
    }
    std::printf("%-21s L=%4zu R=%4zu out=%7zu | median_us NL=%10.3f single=%10.3f joint=%10.3f | candidates=%lld/%lld/%lld\n",
        w.name.c_str(),w.left.size(),w.right.size(),expected.pairs.size(),medians[0],medians[1],medians[2],
        static_cast<long long>(metrics[0].candidates),static_cast<long long>(metrics[1].candidates),static_cast<long long>(metrics[2].candidates));
    std::fflush(stdout); csv.flush(); raw.flush(); CHECK(csv.good() && raw.good()); ++scenario;
  }
  std::printf("PASS: 10 workloads, 30 plan validations, %d measured runs. CSV: %s; raw samples: %s.samples.csv\n",30*repeats,csv_path.c_str(),csv_path.c_str());
}

int main(int argc, char **argv) {
  bool bench = false;
  int rows = 1000, repeats = 7;
  std::string path = "build/hash_join_benchmark.csv";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--bench") bench = true;
    else if (i + 1 < argc && arg == "--rows") rows = std::stoi(argv[++i]);
    else if (i + 1 < argc && arg == "--repeat") repeats = std::stoi(argv[++i]);
    else if (i + 1 < argc && arg == "--csv") path = argv[++i];
    else { std::fprintf(stderr,"Usage: test_hash_join_workloads [--bench --rows 8..2000 --repeat 3..30 --csv path]\n"); return 2; }
  }
  if (rows < 8 || rows > 2000 || repeats < 3 || repeats > 30) { std::fprintf(stderr,"Invalid rows/repeat range\n"); return 2; }
  if (bench) benchmark(rows,repeats,path);
  else {
    for (const auto &w : workloads(40)) { const auto expected = reference(w); for (int a = 0; a < 3; ++a) verify(w,a,expected); }
    std::puts("Hash Join workload tests: 10 scenarios x 3 plans passed (exact multisets, values, refs, RID, counters, lifecycle)");
  }
  return 0;
}
