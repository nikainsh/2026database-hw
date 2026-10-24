/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "common/string_util.h"

#include <cctype>
#include <sstream>

std::string to_lower(const std::string &text)
{
  std::string out = text;
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string to_upper(const std::string &text)
{
  std::string out = text;
  for (char &c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string trim(const std::string &text)
{
  size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string &text, char delimiter)
{
  std::vector<std::string> parts;
  std::string              item;
  std::istringstream       in(text);
  while (std::getline(in, item, delimiter)) {
    const std::string trimmed = trim(item);
    if (!trimmed.empty()) {
      parts.push_back(trimmed);
    }
  }
  return parts;
}

std::string join(const std::vector<std::string> &parts, const std::string &delimiter)
{
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out += delimiter;
    }
    out += parts[i];
  }
  return out;
}
