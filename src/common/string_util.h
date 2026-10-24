/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <string>
#include <vector>

std::string to_lower(const std::string &text);
std::string to_upper(const std::string &text);
/// 去掉首尾空白
std::string trim(const std::string &text);
/// 按分隔符切分，忽略空片段
std::vector<std::string> split(const std::string &text, char delimiter);
/// 用分隔符拼接
std::string join(const std::vector<std::string> &parts, const std::string &delimiter);
