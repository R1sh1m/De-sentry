#pragma once
// Trivial, header-only hex encoding shared by every place that renders raw
// bytes (public keys, ledger hashes, signatures) into a REST-JSON-safe
// string. Kept tiny and dependency-free on purpose -- same philosophy as
// the rest of common/.

#include <string>

namespace desentry {

inline std::string HexEncode(const std::string& bytes) {
  static const char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    hex.push_back(kHex[c >> 4]);
    hex.push_back(kHex[c & 0xF]);
  }
  return hex;
}

}  // namespace desentry
