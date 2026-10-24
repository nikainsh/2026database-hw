/* database26 buffer manager - CRC32 helper for headers and WAL records */
#pragma once

#include <cstddef>
#include <cstdint>

/// Standard CRC-32 (IEEE 802.3, polynomial 0xEDB88320).
uint32_t crc32(const void *data, size_t len);
