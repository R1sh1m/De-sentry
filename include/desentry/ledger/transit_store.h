#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "desentry/common/status.h"
#include "desentry/storage/router.h"

namespace desentry {

inline constexpr char kTransitCollection[] = "__desentry_transit";

struct TransitEnvelope {
  std::string owner_node;
  std::string collection;
  std::string key;
  std::string key_hash;
  std::string encoded_doc;
  std::string holder_node;
  lsn_t intent_lsn = kInvalidLsn;
  int64_t expires_ms = 0;
};

class TransitStore {
 public:
  static StatusOr<std::unique_ptr<TransitStore>> Open(StorageRouter* router,
                                                       uint32_t ttl_seconds,
                                                       std::string holder_node);

  Status Hold(TransitEnvelope envelope);
  std::vector<TransitEnvelope> PendingFor(const std::string& owner_node) const;
  std::vector<std::string> Owners() const;
  StatusOr<size_t> ExpireAsOf(int64_t now_ms);
  size_t Size() const;
  uint64_t BytesHeld() const;
  uint32_t ttl_seconds() const { return ttl_seconds_; }

  void MarkClaimed(const std::string& owner_node, const std::string& key_hash);

 private:
  TransitStore(uint32_t ttl_seconds, std::string holder_node)
      : ttl_seconds_(ttl_seconds), holder_node_(std::move(holder_node)) {}

  uint32_t ttl_seconds_;
  std::string holder_node_;
  mutable std::mutex mutex_;
  std::vector<TransitEnvelope> envelopes_;
};

}  // namespace desentry
