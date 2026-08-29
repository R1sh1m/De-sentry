#include "desentry/common/crc32.h"

#include <array>

namespace desentry {

namespace {
std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}
const std::array<uint32_t, 256> kTable = MakeTable();
}  // namespace

uint32_t Crc32(const void* data, size_t len) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = kTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace desentry
