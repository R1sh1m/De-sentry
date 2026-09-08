#include "desentry/ledger/transit_store.h"

#include <algorithm>

#include "desentry/common/platform.h"

namespace desentry {

StatusOr<std::unique_ptr<TransitStore>> TransitStore::Open(StorageRouter*, uint32_t ttl_seconds,
                                                            std::string holder_node) {
  return std::unique_ptr<TransitStore>(
      new TransitStore(ttl_seconds, std::move(holder_node)));
}

Status TransitStore::Hold(TransitEnvelope envelope) {
  if (envelope.owner_node.empty() || envelope.collection.empty() || envelope.key_hash.empty()) {
    return Status::InvalidArgument("transit envelope is missing required fields");
  }
  if (envelope.expires_ms == 0) {
    envelope.expires_ms = NowMs() + static_cast<int64_t>(ttl_seconds_) * 1000;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = std::find_if(envelopes_.begin(), envelopes_.end(), [&](const TransitEnvelope& item) {
    return item.owner_node == envelope.owner_node && item.key_hash == envelope.key_hash;
  });
  if (existing != envelopes_.end()) {
    *existing = std::move(envelope);
  } else {
    envelopes_.push_back(std::move(envelope));
  }
  return Status::OK();
}

std::vector<TransitEnvelope> TransitStore::PendingFor(const std::string& owner_node) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TransitEnvelope> result;
  for (const TransitEnvelope& envelope : envelopes_) {
    if (envelope.owner_node == owner_node) result.push_back(envelope);
  }
  return result;
}

std::vector<std::string> TransitStore::Owners() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  for (const TransitEnvelope& envelope : envelopes_) {
    if (std::find(result.begin(), result.end(), envelope.owner_node) == result.end()) {
      result.push_back(envelope.owner_node);
    }
  }
  return result;
}

StatusOr<size_t> TransitStore::ExpireAsOf(int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto old_size = envelopes_.size();
  envelopes_.erase(std::remove_if(envelopes_.begin(), envelopes_.end(),
                                  [now_ms](const TransitEnvelope& envelope) {
                                    return envelope.expires_ms <= now_ms;
                                  }),
                   envelopes_.end());
  return old_size - envelopes_.size();
}

size_t TransitStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return envelopes_.size();
}

uint64_t TransitStore::BytesHeld() const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t total = 0;
  for (const TransitEnvelope& envelope : envelopes_) {
    total += envelope.encoded_doc.size();
  }
  return total;
}

void TransitStore::MarkClaimed(const std::string& owner_node, const std::string& key_hash) {
  std::lock_guard<std::mutex> lock(mutex_);
  envelopes_.erase(std::remove_if(envelopes_.begin(), envelopes_.end(),
                                  [&](const TransitEnvelope& envelope) {
                                    return envelope.owner_node == owner_node &&
                                           envelope.key_hash == key_hash;
                                  }),
                   envelopes_.end());
}

}  // namespace desentry
