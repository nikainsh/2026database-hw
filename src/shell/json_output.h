#pragma once

#include <ostream>
#include <string>
#include <vector>
#include "sql/query_engine.h"

// JSON strings preserve separators, newlines and UTF-8; never parse the CLI's ASCII table.
inline void json_string(std::ostream &out, const std::string &value)
{
  const char *hex = "0123456789abcdef";
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else out << static_cast<char>(c);
  }
  out << '"';
}

class JsonOutput : public ResultWriter
{
public:
  RC write_header(const SqlResult &result) override { columns = result.column_names; return RC::SUCCESS; }
  RC write_row(const SqlResult &, const Tuple &tuple) override
  {
    ++total;
    if (rows.size() < 1000) {
      std::vector<std::string> row;
      for (const auto &value : tuple.values) row.push_back(value.to_string());
      rows.push_back(std::move(row));
    }
    return RC::SUCCESS;
  }
  RC write_result(const SqlResult &result) override { affected = result.affected_rows; return RC::SUCCESS; }
  void emit(std::ostream &out, const std::string &sql, RC rc, const SqlResult &result,
            const std::string &profile) const
  {
    out << "{\"sql\":"; json_string(out, sql);
    out << ",\"code\":"; json_string(out, strrc(rc));
    out << ",\"message\":"; json_string(out, result.hint ? "Use the SQL examples in the GUI." : result.message);
    out << ",\"profile\":"; json_string(out, profile);
    out << ",\"time_us\":" << result.execution_time_us << ",\"total\":" << total
        << ",\"affected\":" << affected << ",\"columns\":[";
    for (size_t i = 0; i < columns.size(); ++i) { if (i) out << ','; json_string(out, columns[i]); }
    out << "],\"rows\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i) out << ',';
      out << '[';
      for (size_t j = 0; j < rows[i].size(); ++j) { if (j) out << ','; json_string(out, rows[i][j]); }
      out << ']';
    }
    out << "]}";
  }
private:
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  int64_t total = 0, affected = 0;
};
