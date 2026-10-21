/* minidb - a minimal relational DBMS, architecture follows oceanbase/miniob */

#pragma once

#include "common/types.h"

/// 页大小固定 8KB：数据文件与索引文件都按页组织，页号从 0 开始
static constexpr int BP_PAGE_SIZE = 8 * 1024;

/// 磁盘上的一个页面，只有裸数据；脏、pin、页号等信息保存在 Frame 中
struct Page
{
  char data[BP_PAGE_SIZE];
};
