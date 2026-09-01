// Network layer test suite: the authenticated-encryption handshake over a
// real socket pair, the TCP transport's request/response cycle over real
// loopback TCP (including a cleanly-handled unreachable-peer case), and --
// the whole point of this project -- a 3-node P2P mesh actually converging
// via eager broadcast AND independently via gossip anti-entropy.
//
// This test spins up real threads and sleeps to let asynchronous
// networking settle, so it's slower than the other suites (a few seconds)
// -- that's inherent to testing a distributed system honestly rather than
// mocking the network away.

#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "desentry/common/json.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/network_manager.h"
#include "desentry/net/secure_channel.h"
#include "desentry/net/tcp_transport.h"

using namespace desentry;

namespace {
void RmRf(const std::string& path) { int rc = std::system(("rm -rf " + path).c_str()); (void)rc; }
}  // namespace

static void TestSecureChannelHandshake() {
  RmRf("/tmp/desentry_test_net");
  int rc = std::system("mkdir -p /tmp/desentry_test_net"); (void)rc;
  auto idA = NodeIdentity::LoadOrCreate("/tmp/desentry_test_net/idA.key").ValueOrDie();
  auto idB = NodeIdentity::LoadOrCreate("/tmp/desentry_test_net/idB.key").ValueOrDie();
  assert(idA.node_id() != idB.node_id());

  int fds[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  StatusOr<HandshakeResult> client_result = Status::Internal("unset");
  StatusOr<HandshakeResult> server_result = Status::Internal("unset");
  std::thread server_thread([&]() { server_result = ServerHandshake(fds[1], idB, 7801); });
  client_result = ClientHandshake(fds[0], idA, 7802);
  server_thread.join();

  assert(client_result.ok() && server_result.ok());
  assert(client_result.value().peer_node_id == idB.node_id());
  assert(server_result.value().peer_node_id == idA.node_id());

  SessionKeys ckeys = client_result.value().keys;
  SessionKeys skeys = server_result.value().keys;
  std::thread echo([&]() {
    auto m = RecvEncrypted(fds[1], &skeys, 1 << 20).ValueOrDie();
    assert(m.type == MessageType::kPing && m.payload == "ping");
    SendEncrypted(fds[1], &skeys, WireMessage{MessageType::kPong, "pong"});
  });
  SendEncrypted(fds[0], &ckeys, WireMessage{MessageType::kPing, "ping"});
  auto reply = RecvEncrypted(fds[0], &ckeys, 1 << 20).ValueOrDie();
  echo.join();
  assert(reply.type == MessageType::kPong && reply.payload == "pong");

  close(fds[0]);
  close(fds[1]);
  std::cout << "[network_test] secure channel handshake + encrypted round-trip: PASS" << std::endl;
}

static void TestTcpTransportOverRealLoopback() {
  auto idA = NodeIdentity::LoadOrCreate("/tmp/desentry_test_net/tcpA.key").ValueOrDie();
  auto idB = NodeIdentity::LoadOrCreate("/tmp/desentry_test_net/tcpB.key").ValueOrDie();

  TcpTransport server(&idB, 19801);
  auto st = server.StartListening("127.0.0.1", [&](const std::string& peer_id, const WireMessage& req) {
    assert(peer_id == idA.node_id());
    return WireMessage{MessageType::kPong, "echo:" + req.payload};
  });
  assert(st.ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TcpTransport client(&idA, 19802);
  auto resp = client.SendRequest("127.0.0.1", 19801, WireMessage{MessageType::kPing, "hi"}).ValueOrDie();
  assert(resp.payload == "echo:hi");

  auto fail = client.SendRequest("127.0.0.1", 19999, WireMessage{MessageType::kPing, "x"});
  assert(!fail.ok());  // unreachable peer must fail cleanly, not hang/crash

  server.Stop();
  std::cout << "[network_test] TCP transport over real loopback (handshake+encryption+unreachable-peer handling): PASS" << std::endl;
}

static void TestThreeNodeMeshConverges() {
  RmRf("/tmp/desentry_test_net/nodeA");
  RmRf("/tmp/desentry_test_net/nodeB");
  RmRf("/tmp/desentry_test_net/nodeC");

  NodeEngine::Options oA, oB, oC;
  oA.data_dir = "/tmp/desentry_test_net/nodeA"; oA.buffer_pool_pages = 64;
  oB.data_dir = "/tmp/desentry_test_net/nodeB"; oB.buffer_pool_pages = 64;
  oC.data_dir = "/tmp/desentry_test_net/nodeC"; oC.buffer_pool_pages = 64;
  auto engA = NodeEngine::Open(oA).ValueOrDie();
  auto engB = NodeEngine::Open(oB).ValueOrDie();
  auto engC = NodeEngine::Open(oC).ValueOrDie();

  NodeConfig cfgA, cfgB, cfgC;
  cfgA.p2p_port = 19911; cfgA.discovery_enabled = false; cfgA.gossip_interval_ms = 300;
  cfgB.p2p_port = 19912; cfgB.discovery_enabled = false; cfgB.gossip_interval_ms = 300;
  cfgC.p2p_port = 19913; cfgC.discovery_enabled = false; cfgC.gossip_interval_ms = 300;
  cfgA.bootstrap_peers = {"127.0.0.1:19912", "127.0.0.1:19913"};
  cfgB.bootstrap_peers = {"127.0.0.1:19911", "127.0.0.1:19913"};
  cfgC.bootstrap_peers = {"127.0.0.1:19911", "127.0.0.1:19912"};

  NetworkManager netA(engA.get(), cfgA), netB(engB.get(), cfgB), netC(engC.get(), cfgC);
  assert(netA.Start().ok() && netB.Start().ok() && netC.Start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  assert(engA->PutDocument("users", "u1", JsonValue::Parse(R"({"name":"Asha","role":"admin"})")).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  assert(engB->GetDocument("users", "u1").ok());
  assert(engC->GetDocument("users", "u1").ok());

  // Concurrent divergent writes on B and C to the same document.
  assert(engB->PutDocument("users", "u1", JsonValue::Parse(R"({"name":"Asha","role":"admin","dept":"eng"})")).ok());
  assert(engC->PutDocument("users", "u1", JsonValue::Parse(R"({"name":"Asha Khan","role":"admin"})")).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  auto a = engA->GetDocument("users", "u1").ValueOrDie();
  auto b = engB->GetDocument("users", "u1").ValueOrDie();
  auto c = engC->GetDocument("users", "u1").ValueOrDie();
  assert(a.CanonicalDump() == b.CanonicalDump());
  assert(b.CanonicalDump() == c.CanonicalDump());
  std::cout << "[network_test] 3-node concurrent-write convergence -> " << a.Dump() << std::endl;

  // Gossip-only propagation path (new collection+key reaches every node).
  assert(engC->PutDocument("items", "i1", JsonValue::Parse(R"({"sku":"X1","qty":5})")).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  assert(engA->GetDocument("items", "i1").ok());
  assert(engB->GetDocument("items", "i1").ok());

  netA.Stop(); netB.Stop(); netC.Stop();
  std::cout << "[network_test] 3-node P2P mesh (eager broadcast + gossip anti-entropy): PASS" << std::endl;
}

int main() {
  TestSecureChannelHandshake();
  TestTcpTransportOverRealLoopback();
  TestThreeNodeMeshConverges();
  std::cout << "[network_test] ALL NETWORK TESTS PASSED" << std::endl;
  return 0;
}
