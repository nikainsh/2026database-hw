/* database26 buffer manager - CRC32 helper for headers and WAL records */
#include "storage/buffer/checksum.h"

#include <array>
#include <mutex>

namespace
{
constexpr uint32_t kPolynomial = 0xEDB88320u;

const std::array<uint32_t, 256> &crc_table()
{
  static std::array<uint32_t, 256> table{};
  static std::once_flag            once;
  std::call_once(once, []() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? (value >> 1) ^ kPolynomial : (value >> 1);
      }
      table[i] = value;
    }
  });
  return table;
}
}  // namespace

uint32_t crc32(const void *data, size_t len)
{
  const auto    *bytes = static_cast<const uint8_t *>(data);
  const auto    &table = crc_table();
  uint32_t       crc   = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}
