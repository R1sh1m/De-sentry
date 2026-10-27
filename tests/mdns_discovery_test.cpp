#include <cassert>
#include <cstdint>
#include <string>

#include "desentry/common/byte_buffer.h"
#include "desentry/net/mdns_discovery.h"

namespace {

// Test packet encoding/decoding without network access.
void TestPacketFormat() {
  const std::string node_id(32, 'A');  // 32-byte node_id
  const uint16_t p2p_port = 7801;
  const uint16_t api_port = 7701;

  // Encode
  desentry::ByteWriter w;
  w.U32(0x4D445352);  // "MDSR" magic
  w.Bytes(node_id);
  w.U16(p2p_port);
  w.U16(api_port);
  std::string encoded = w.TakeString();

  // Verify size: 4 (magic) + 4 (length uint32) + 32 (node_id) + 2 + 2 = 44
  assert(encoded.size() == 44);

  // Decode
  desentry::ByteReader r(encoded.data(), encoded.size());
  uint32_t magic = r.U32();
  assert(magic == 0x4D445352);

  std::string decoded_node_id = r.Bytes();
  assert(decoded_node_id == node_id);

  uint16_t decoded_p2p = r.U16();
  assert(decoded_p2p == p2p_port);

  uint16_t decoded_api = r.U16();
  assert(decoded_api == api_port);

  assert(r.remaining() == 0);
}

void TestCallbackInvocation() {
  bool called = false;
  std::string captured_node_id;
  std::string captured_host;
  uint16_t captured_p2p = 0;
  uint16_t captured_api = 0;

  desentry::MdnsDiscovery mdns([&](const std::string& node_id, const std::string& host,
                                    uint16_t p2p_port, uint16_t api_port) {
    called = true;
    captured_node_id = node_id;
    captured_host = host;
    captured_p2p = p2p_port;
    captured_api = api_port;
  });

  // We can't test Start/Stop without network, but we can verify the callback
  // type is correct and the object constructs.
  assert(!called);
  (void)mdns;  // suppress unused warning
}

}  // namespace

int main() {
  TestPacketFormat();
  TestCallbackInvocation();
  return 0;
}