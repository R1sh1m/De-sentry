#include <cassert>
#include <iostream>
#include <string>

#include "desentry/net/relay_client.h"

using namespace desentry;

static void TestFrameEncodeDecodeRoundTrip() {
  const std::string payload = "hello, relay!";
  const std::string frame = RelayClient::EncodeFrame(payload);
  auto decoded = RelayClient::DecodeFrame(frame);
  assert(decoded.ok());
  assert(decoded.value() == payload);
  std::cout << "[relay_client_test] frame encode/decode round-trip: PASS" << std::endl;
}

static void TestFrameEmptyPayload() {
  const std::string payload = "";
  const std::string frame = RelayClient::EncodeFrame(payload);
  auto decoded = RelayClient::DecodeFrame(frame);
  assert(decoded.ok());
  assert(decoded.value() == payload);
  std::cout << "[relay_client_test] empty payload round-trip: PASS" << std::endl;
}

static void TestFrameLargePayload() {
  std::string payload(10000, 'x');
  const std::string frame = RelayClient::EncodeFrame(payload);
  auto decoded = RelayClient::DecodeFrame(frame);
  assert(decoded.ok());
  assert(decoded.value() == payload);
  std::cout << "[relay_client_test] large payload round-trip: PASS" << std::endl;
}

static void TestFrameTruncated() {
  const std::string frame = "ab";  // too short
  auto decoded = RelayClient::DecodeFrame(frame);
  assert(!decoded.ok());
  std::cout << "[relay_client_test] truncated frame rejected: PASS" << std::endl;
}

int main() {
  TestFrameEncodeDecodeRoundTrip();
  TestFrameEmptyPayload();
  TestFrameLargePayload();
  TestFrameTruncated();
  std::cout << "[relay_client_test] ALL RELAY CLIENT TESTS PASSED" << std::endl;
  return 0;
}
