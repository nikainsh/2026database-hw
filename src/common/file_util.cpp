/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#include "common/file_util.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0755)
#endif

bool file_exist(const std::string &path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

int64_t file_size(const std::string &path)
{
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return -1;
  }
  return static_cast<int64_t>(st.st_size);
}

std::string path_normalize(const std::string &path)
{
  std::string out = path;
  for (char &c : out) {
    if (c == '\\') {
      c = '/';
    }
  }
  return out;
}

std::string path_join(const std::string &dir, const std::string &name)
{
  if (dir.empty()) {
    return name;
  }
  const char last = dir.back();
  if (last == '/' || last == '\\') {
    return dir + name;
  }
  return dir + "/" + name;
}

RC path_mkdir_recursive(const std::string &raw_path)
{
  const std::string path = path_normalize(raw_path);

  std::vector<std::string> parts;
  size_t                   pos = 0;
  while (pos < path.size()) {
    size_t      slash = path.find('/', pos);
    std::string part  = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
    if (!part.empty()) {
      parts.push_back(part);
    }
    if (slash == std::string::npos) {
      break;
    }
    pos = slash + 1;
  }
  if (parts.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  std::string current = path[0] == '/' ? "/" : "";
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string &part = parts[i];
    if (i == 0 && part.size() == 2 && part[1] == ':') {
      current = part + "/";  /// Windows 盘符，如 "E:"
      continue;
    }
    if (part == ".") {
      continue;
    }

    current = path_join(current, part);

    struct stat st;
    if (stat(current.c_str(), &st) == 0) {
      if (!S_ISDIR(st.st_mode)) {
        return RC::FILE_CREATE;
      }
      continue;
    }

    if (MKDIR(current.c_str()) != 0) {
      struct stat retry;
      if (stat(current.c_str(), &retry) != 0 || !S_ISDIR(retry.st_mode)) {
        return RC::FILE_CREATE;
      }
    }
  }
  return RC::SUCCESS;
}

std::vector<std::string> path_list_files(const std::string &dir)
{
  std::vector<std::string> files;
  DIR                     *d = opendir(dir.c_str());
  if (d == nullptr) {
    return files;
  }

  const std::string prefix = path_normalize(dir);
  struct dirent    *item   = nullptr;
  while ((item = readdir(d)) != nullptr) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    const std::string full = path_join(prefix, name);

    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      continue;
    }
    files.push_back(full);
  }
  closedir(d);
  return files;
}

RC path_remove_file(const std::string &path)
{
  if (!file_exist(path)) {
    return RC::FILE_NOT_EXIST;
  }
  return remove(path.c_str()) == 0 ? RC::SUCCESS : RC::FILE_REMOVE;
}
