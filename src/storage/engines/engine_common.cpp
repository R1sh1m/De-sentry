#include "desentry/storage/engines/engine_common.h"

#include <algorithm>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"

namespace desentry {

namespace {
thread_local int g_merge_depth = 0;
}  // namespace

MergeScope::MergeScope() { ++g_merge_depth; }
MergeScope::~MergeScope() { --g_merge_depth; }

bool BaseBackend::InMergeScope() { return g_merge_depth > 0; }

Status BaseBackend::MergeRemote(const std::string& collection, const std::string& key,
                                 const std::string& remote_encoded_doc) {
  MergeScope scope;

  CrdtValue remote;
  try {
    remote = DecodeDocument(remote_encoded_doc);
  } catch (const std::exception& e) {
    // A malformed document from a peer is untrusted input, not a bug here.
    return Status::Corruption(std::string("undecodable remote document: ") + e.what());
  }

  auto existing = Get(collection, key);
  CrdtValue merged;
  if (existing.ok()) {
    CrdtValue local;
    try {
      local = DecodeDocument(existing.value());
    } catch (const std::exception& e) {
      return Status::Corruption(std::string("undecodable local document: ") + e.what());
    }
    merged = CrdtValue::Merge(local, remote);
  } else if (existing.status().code() == StatusCode::kNotFound) {
    merged = remote;
  } else {
    return existing.status();
  }

  return Put(collection, key, EncodeDocument(merged));
}

std::string BaseBackend::Checksum(const std::string& collection) {
  return FingerprintRows(Scan(collection, "", 0));
}

std::string FingerprintRows(const std::vector<EngineRow>& rows) {
  std::vector<std::string> fingerprints;
  fingerprints.reserve(rows.size());
  for (const auto& [key, bytes] : rows) {
    CrdtValue doc;
    try {
      doc = DecodeDocument(bytes);
    } catch (const std::exception&) {
      // An unreadable row is a real divergence signal, not something to
      // silently skip: fold its raw hash in so two peers disagree loudly
      // rather than appearing converged.
      fingerprints.push_back(key + "=!corrupt:" + HexEncode(crypto::Sha256(bytes)));
      continue;
    }
    if (doc.IsEmpty()) continue;  // tombstoned -- not a live document
    fingerprints.push_back(key + "=" + doc.MaxTimestamp().ToString());
  }
  // Sorted so the digest is independent of physical scan order: two nodes
  // that converged produce the same digest regardless of how their storage
  // happens to be laid out. This is the same rule NodeEngine::Summarize()
  // follows, deliberately -- see engine_common.h.
  std::sort(fingerprints.begin(), fingerprints.end());
  std::string joined;
  for (const std::string& f : fingerprints) {
    joined += f;
    joined += '\n';
  }
  return HexEncode(crypto::Sha256(joined));
}

Status BaseBackend::Charge(uint64_t old_cost, uint64_t new_cost) {
  if (new_cost <= old_cost) {
    uint64_t freed = old_cost - new_cost;
    uint64_t current = bytes_used_.load();
    for (;;) {
      uint64_t next = freed > current ? 0 : current - freed;
      if (bytes_used_.compare_exchange_weak(current, next)) return Status::OK();
    }
  }

  const uint64_t growth = new_cost - old_cost;
  if (quota_bytes_ == 0) {  // unlimited
    bytes_used_.fetch_add(growth);
    return Status::OK();
  }

  uint64_t ceiling = quota_bytes_;
  if (InMergeScope()) {
    ceiling += quota_bytes_ / 100 * kMergeOverdraftPercent;
  }

  // Compare-and-swap rather than "load, check, add": two concurrent writers
  // each seeing headroom for their own write could otherwise both commit
  // and overshoot the budget.
  uint64_t current = bytes_used_.load();
  for (;;) {
    if (current + growth > ceiling) {
      return Status::OutOfSpace("quota exceeded: " + std::to_string(current + growth) + " > " +
                                 std::to_string(ceiling) + " bytes");
    }
    if (bytes_used_.compare_exchange_weak(current, current + growth)) {
      if (InMergeScope() && current + growth > quota_bytes_) {
        DSN_LOG_WARN("quota", "replication merge overdrew quota (" << (current + growth) << " > "
                                                                    << quota_bytes_ << " bytes)");
      }
      return Status::OK();
    }
  }
}

}  // namespace desentry
