#pragma once
#include <cstddef>
#include <cstdint>

namespace desentry {

// Standard CRC-32 (IEEE 802.3 polynomial), used to detect torn/corrupt WAL
// records and corrupted page/document payloads. Implemented locally (no
// zlib dependency) since it's ~20 lines and keeps the build dependency-free.
uint32_t Crc32(const void* data, size_t len);

}  // namespace desentry
