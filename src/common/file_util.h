/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/rc.h"

bool        file_exist(const std::string &path);
int64_t     file_size(const std::string &path);
std::string path_join(const std::string &dir, const std::string &name);
/// 逐级创建目录，已存在时返回 SUCCESS
RC path_mkdir_recursive(const std::string &path);
/// 列出目录下所有普通文件（返回完整路径）
std::vector<std::string> path_list_files(const std::string &dir);
RC path_remove_file(const std::string &path);
/// 把 '\' 统一成 '/'，便于比较与打印
std::string path_normalize(const std::string &path);
