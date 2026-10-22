/* minidb - 用户界面（命令行 REPL），对应 miniob 的 obclient + observer 交互部分 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/log.h"
#include "common/rc.h"
#include "common/string_util.h"
#include "sql/query_engine.h"
#include "storage/db/db.h"

namespace {

const char *kHelpText = R"(minidb supports:
  create table t (id int, name char(16), age int, score float, primary key(id))
  drop table [if exists] t
  create [unique] index i on t (col)
  show tables
  desc | describe t
  insert into t (id, name, age) values (1, 'a', 20)
  insert into t values (1, 'a', 20, 1.5), (2, 'b', 21, 2.5)
  delete from t where id = 1 [and/or ...]
  update t set age = 22 where id = 1
  select * | col, count(*), sum(x) from t1 [alias], t2 where cond [group by col]
  explain select ...
  help | exit | quit
Comments: -- line comment, and /* block comment */.
)";

/// 切分结果：完整语句 + 还没写完的尾巴（REPL 需要据此决定是否继续读一行）
struct SplitResult
{
  std::vector<std::string> complete;
  std::string              pending;
};

/// 按分号切分多条语句，忽略字符串与注释里的分号
SplitResult split_partial(const std::string &text)
{
  SplitResult result;
  std::string current;
  bool        in_string = false;
  bool        in_line   = false;
  bool        in_block  = false;

  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];

    if (in_line) {
      current.push_back(c);
      if (c == '\n') {
        in_line = false;
      }
      continue;
    }
    if (in_block) {
      current.push_back(c);
      if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
        current.push_back('/');
        in_block = false;
        ++i;
      }
      continue;
    }
    if (in_string) {
      current.push_back(c);
      if (c == '\'') {
        if (i + 1 < text.size() && text[i + 1] == '\'') {
          current.push_back('\'');
          ++i;
        } else {
          in_string = false;
        }
      }
      continue;
    }

    if (c == '\'') {
      in_string = true;
      current.push_back(c);
      continue;
    }
    if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
      in_line = true;
      current.push_back(c);
      continue;
    }
    if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      in_block = true;
      current.push_back(c);
      continue;
    }
    if (c == ';') {
      if (!trim(current).empty()) {
        result.complete.push_back(current);
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }

  if (!trim(current).empty()) {
    result.pending = current;
  }
  return result;
}

std::vector<std::string> split_statements(const std::string &text)
{
  SplitResult parts = split_partial(text);
  if (!parts.pending.empty()) {
    parts.complete.push_back(parts.pending);
  }
  return parts.complete;
}

/// 把查询结果排成对齐的表格打印出来
class TableOutput : public ResultWriter
{
public:
  RC write_header(const SqlResult &result) override
  {
    names_ = result.column_names;
    rows_.clear();
    widths_.assign(names_.size(), 0);
    for (size_t i = 0; i < names_.size(); ++i) {
      widths_[i] = static_cast<int>(names_[i].size());
    }
    return RC::SUCCESS;
  }

  RC write_row(const SqlResult &, const Tuple &tuple) override
  {
    std::vector<std::string> row;
    for (size_t i = 0; i < tuple.values.size(); ++i) {
      row.push_back(tuple.values[i].to_string());
    }
    for (size_t i = 0; i < row.size() && i < widths_.size(); ++i) {
      widths_[i] = std::max(widths_[i], static_cast<int>(row[i].size()));
    }
    rows_.push_back(row);
    return RC::SUCCESS;
  }

  RC write_result(const SqlResult &result) override
  {
    if (!result.is_query) {
      return RC::SUCCESS;
    }
    if (rows_.empty()) {
      std::printf("(no rows)\n");
      return RC::SUCCESS;
    }

    std::string separator;
    for (size_t i = 0; i < widths_.size(); ++i) {
      separator += std::string(static_cast<size_t>(widths_[i] + 2), '-');
      if (i + 1 < widths_.size()) {
        separator += "+";
      }
    }

    auto print_line = [&](const std::vector<std::string> &cells) {
      for (size_t i = 0; i < widths_.size(); ++i) {
        const std::string cell = i < cells.size() ? cells[i] : std::string();
        std::printf(" %-*s |", widths_[i], cell.c_str());
      }
      std::printf("\n");
    };

    print_line(names_);
    std::printf("%s\n", separator.c_str());
    for (const auto &row : rows_) {
      print_line(row);
    }
    std::printf("%zu rows\n", rows_.size());
    return RC::SUCCESS;
  }

private:
  std::vector<std::string>       names_;
  std::vector<std::vector<std::string>> rows_;
  std::vector<int>               widths_;
};

/// 执行类语句（DDL/DML）时只打印一句话结果
class MessageOutput : public ResultWriter
{
public:
  explicit MessageOutput(bool show_rows) : show_rows_(show_rows) {}

  RC write_header(const SqlResult &result) override
  {
    if (show_rows_ && result.is_query) {
      plain_.write_header(result);
    }
    return RC::SUCCESS;
  }
  RC write_row(const SqlResult &result, const Tuple &tuple) override
  {
    if (show_rows_ && result.is_query) {
      plain_.write_row(result, tuple);
    }
    return RC::SUCCESS;
  }
  RC write_result(const SqlResult &result) override
  {
    if (show_rows_ && result.is_query) {
      return plain_.write_result(result);
    }
    return RC::SUCCESS;
  }

private:
  bool         show_rows_ = true;
  TableOutput  plain_;
};

struct Options
{
  std::string db_name    = "testdb";
  std::string data_dir   = "./data";
  std::string sql_file;
  std::string execute_sql;
  bool        verbose    = false;
  bool        show_rows  = true;
  bool        print_help = false;
};

Options parse_args(int argc, char **argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--db" && i + 1 < argc) {
      options.db_name = argv[++i];
    } else if (arg == "--data" && i + 1 < argc) {
      options.data_dir = argv[++i];
    } else if (arg == "-f" && i + 1 < argc) {
      options.sql_file = argv[++i];
    } else if (arg == "-e" && i + 1 < argc) {
      options.execute_sql = argv[++i];
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--quiet") {
      options.show_rows = false;
    } else if (arg == "-h" || arg == "--help") {
      options.print_help = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      options.print_help = true;
    }
  }
  return options;
}

void usage()
{
  std::printf(
      "usage: minidb [options]\n"
      "  --db <name>     database name (default: testdb)\n"
      "  --data <dir>    data directory (default: ./data)\n"
      "  -f <file>       run a SQL script and exit\n"
      "  -e <sql>        run one statement and exit\n"
      "  --verbose       print debug logs\n"
      "  --quiet         do not print result rows\n"
      "  -h, --help      show this help\n");
}

/// 执行一条语句并打印结果；返回是否要继续
bool run_statement(QueryEngine &engine, const std::string &sql, ResultWriter &writer, bool &want_exit)
{
  want_exit = false;
  if (trim(sql).empty()) {
    return true;
  }

  SqlResult result;
  RC        rc = engine.execute(sql, writer, result);

  if (result.want_exit) {
    want_exit = true;
    return false;
  }
  if (result.hint != nullptr) {
    std::printf("%s", kHelpText);
    return true;
  }

  if (OB_FAIL(rc)) {
    std::printf("ERROR [%s] %s\n", strrc(rc), result.message.c_str());
    return false;
  }
  if (!result.message.empty() && !(result.is_query)) {
    std::printf("%s (%lld us)\n", result.message.c_str(), (long long)result.execution_time_us);
  } else if (result.is_query) {
    std::printf("  (%lld us)\n", (long long)result.execution_time_us);
  }
  return true;
}

}  // namespace

int main(int argc, char **argv)
{
  const Options options = parse_args(argc, argv);
  if (options.print_help) {
    usage();
    return 0;
  }

  log_set_level(options.verbose ? LogLevel::DBG : LogLevel::WARN);

  path_mkdir_recursive(options.data_dir);

  Db *db = nullptr;
  RC  rc = Db::open(options.data_dir, options.db_name, &db);
  if (rc == RC::FILE_NOT_EXIST || rc == RC::SCHEMA_DB_NOT_EXIST) {
    rc = Db::create(options.data_dir, options.db_name, &db);
    if (OB_FAIL(rc)) {
      std::fprintf(stderr, "failed to create db '%s': %s\n", options.db_name.c_str(), strrc(rc));
      return 1;
    }
    std::printf("created a new database '%s' in %s\n", options.db_name.c_str(), options.data_dir.c_str());
  } else if (OB_FAIL(rc)) {
    std::fprintf(stderr, "failed to open db '%s': %s\n", options.db_name.c_str(), strrc(rc));
    return 1;
  }

  QueryEngine     engine(db);
  TableOutput     writer;
  MessageOutput   quiet_writer(options.show_rows);
  ResultWriter   &out = options.show_rows ? static_cast<ResultWriter &>(writer) : static_cast<ResultWriter &>(quiet_writer);

  bool ok = true;

  if (!options.execute_sql.empty()) {
    bool want_exit = false;
    ok             = run_statement(engine, options.execute_sql, out, want_exit);
  } else if (!options.sql_file.empty()) {
    std::ifstream in(options.sql_file);
    if (!in) {
      std::fprintf(stderr, "failed to open sql file: %s\n", options.sql_file.c_str());
      db->close();
      delete db;
      return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    for (const std::string &statement : split_statements(buffer.str())) {
      std::printf("minidb> %s\n", trim(statement).c_str());
      bool want_exit = false;
      if (!run_statement(engine, statement, out, want_exit)) {
        ok = !want_exit;  /// exit 不算失败
        break;
      }
    }
  } else {
    std::printf("minidb [%s] -- type 'help' for the supported syntax, 'exit' to quit\n",
                options.db_name.c_str());
    std::string line, buffer_text;
    while (true) {
      std::printf(buffer_text.empty() ? "minidb> " : "    ...> ");
      std::fflush(stdout);
      if (!std::getline(std::cin, line)) {
        break;  /// EOF
      }
      buffer_text += line;
      buffer_text += "\n";

      SplitResult parts    = split_partial(buffer_text);

      /// help / exit / quit 通常不写分号，遇到就立刻执行，不要一直等下去
      const std::string pending_lower = to_lower(trim(parts.pending));
      if (pending_lower == "help" || pending_lower == "exit" || pending_lower == "quit") {
        parts.complete.push_back(parts.pending);
        parts.pending.clear();
      }

      bool finished = false;
      for (const std::string &statement : parts.complete) {
        bool want_exit = false;
        if (!run_statement(engine, statement, out, want_exit)) {
          if (want_exit) {
            finished = true;
          } else {
            ok = false;
          }
          break;
        }
      }
      if (finished) {
        break;
      }
      /// 没写完的尾巴留到下一次输入
      buffer_text = parts.pending;
    }
    std::printf("\n");
  }

  RC close_rc = db->close();
  if (OB_FAIL(close_rc)) {
    std::fprintf(stderr, "failed to close db: %s\n", strrc(close_rc));
    ok = false;
  }
  delete db;
  return ok ? 0 : 1;
}
