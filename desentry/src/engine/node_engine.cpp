#include "desentry/engine/node_engine.h"

#include <sys/stat.h>

#include <cerrno>

#include <algorithm>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"

namespace desentry {

StatusOr<std::unique_ptr<NodeEngine>> NodeEngine::Open(const Options& options) {
  std::unique_ptr<NodeEngine> engine(new NodeEngine());

  auto id_or = NodeIdentity::LoadOrCreate(options.data_dir + "/identity.key");
  // identity.key must exist before we can even name the data directory's
  // WAL/data files in log lines usefully, but the directory itself may not
  // exist yet on first boot -- create it first.
  if (!id_or.ok()) {
    if (::mkdir(options.data_dir.c_str(), 0755) == 0 || errno == EEXIST) {
      id_or = NodeIdentity::LoadOrCreate(options.data_dir + "/identity.key");
    }
  }
  if (!id_or.ok()) return id_or.status();
  engine->identity_ = std::make_unique<NodeIdentity>(id_or.value());
  engine->clock_ = std::make_unique<HybridLogicalClock>(engine->identity_->node_id());

  StorageEngine::Options storage_opts;
  storage_opts.data_dir = options.data_dir;
  storage_opts.buffer_pool_pages = options.buffer_pool_pages;
  auto storage_or = StorageEngine::Open(storage_opts);
  if (!storage_or.ok()) return storage_or.status();
  engine->storage_ = std::move(storage_or.value());

  DSN_LOG_INFO("engine", "node engine ready, node_id=" << engine->identity_->node_id());
  return engine;
}

Status NodeEngine::PutDocument(const std::string& collection, const std::string& key, const JsonValue& new_json) {
  auto existing_or = storage_->GetRaw(collection, key);
  CrdtValue previous;
  bool had_previous = false;
  if (existing_or.ok()) {
    previous = DecodeDocument(existing_or.value());
    had_previous = true;
  } else if (existing_or.status().code() != StatusCode::kNotFound) {
    return existing_or.status();
  }

  HLCTimestamp ts = clock_->Now();
  CrdtValue updated = had_previous ? CrdtValue::ApplyJsonUpdate(previous, new_json, ts)
                                    : CrdtValue::FromJson(new_json, ts);
  std::string encoded = EncodeDocument(updated);

  Status st = storage_->PutRaw(collection, key, encoded);
  if (!st.ok()) return st;

  if (on_local_write_) on_local_write_(collection, key, encoded);
  return Status::OK();
}

Status NodeEngine::DeleteDocument(const std::string& collection, const std::string& key) {
  return PutDocument(collection, key, JsonValue::MakeObject());
}

StatusOr<JsonValue> NodeEngine::GetDocument(const std::string& collection, const std::string& key) {
  auto raw_or = storage_->GetRaw(collection, key);
  if (!raw_or.ok()) return raw_or.status();
  CrdtValue doc = DecodeDocument(raw_or.value());
  if (doc.IsEmpty()) return Status::NotFound("document deleted: " + key);
  return doc.ToJson();
}

std::vector<std::pair<std::string, JsonValue>> NodeEngine::ListDocuments(const std::string& collection,
                                                                            const std::string& start_key,
                                                                            size_t limit) {
  std::vector<std::pair<std::string, JsonValue>> out;
  // Over-fetch to account for tombstoned (deleted-but-still-indexed)
  // documents we'll filter out below, so a caller asking for `limit` live
  // documents doesn't get a short page just because some keys in that
  // range happen to be deleted.
  size_t fetch_limit = limit == 0 ? 0 : limit * 2 + 16;
  auto raw = storage_->Scan(collection, start_key, fetch_limit);
  for (auto& [key, bytes] : raw) {
    CrdtValue doc = DecodeDocument(bytes);
    if (doc.IsEmpty()) continue;
    out.emplace_back(key, doc.ToJson());
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

Status NodeEngine::MergeRemote(const std::string& collection, const std::string& key,
                                const std::string& remote_encoded_doc) {
  CrdtValue remote = DecodeDocument(remote_encoded_doc);
  clock_->Observe(remote.MaxTimestamp());  // causality: our next Now() will be after this write

  auto existing_or = storage_->GetRaw(collection, key);
  CrdtValue merged;
  if (existing_or.ok()) {
    CrdtValue local = DecodeDocument(existing_or.value());
    merged = CrdtValue::Merge(local, remote);
  } else if (existing_or.status().code() == StatusCode::kNotFound) {
    merged = remote;
  } else {
    return existing_or.status();
  }

  // Deliberately does NOT invoke on_local_write_: rebroadcasting every
  // merged remote write would relay it to every peer we're connected to,
  // including the one we just got it from, causing broadcast storms in any
  // mesh bigger than 2 nodes. This is why the MVP's gossip anti-entropy
  // round (net/gossip.h) -- not eager rebroadcast -- is what guarantees
  // eventual convergence across a >2-node mesh; full multi-hop eager relay
  // (with de-duplication) is a stated roadmap item, not silently assumed.
  return storage_->PutRaw(collection, key, EncodeDocument(merged));
}

std::vector<DigestEntryOut> NodeEngine::LocalDigest(const std::string& collection) {
  std::vector<DigestEntryOut> out;
  auto raw = storage_->Scan(collection, "", 0);
  out.reserve(raw.size());
  for (auto& [key, bytes] : raw) {
    CrdtValue doc = DecodeDocument(bytes);
    out.push_back(DigestEntryOut{key, doc.MaxTimestamp()});
  }
  return out;
}

std::vector<std::string> NodeEngine::ListCollections() { return storage_->ListCollections(); }

StatusOr<std::string> NodeEngine::GetRawEncoded(const std::string& collection, const std::string& key) {
  return storage_->GetRaw(collection, key);
}

CollectionSummary NodeEngine::Summarize(const std::string& collection) {
  CollectionSummary summary;
  summary.name = collection;
  auto raw = storage_->Scan(collection, "", 0);
  std::vector<std::string> fingerprints;
  fingerprints.reserve(raw.size());
  for (auto& [key, bytes] : raw) {
    CrdtValue doc = DecodeDocument(bytes);
    if (doc.IsEmpty()) continue;  // tombstoned -- not a live document
    summary.document_count++;
    fingerprints.push_back(key + "=" + doc.MaxTimestamp().ToString());
  }
  // Sorted so the checksum is independent of physical scan order -- two
  // nodes that have converged to the same set of documents produce the
  // same checksum regardless of how their B+Trees happen to be laid out.
  std::sort(fingerprints.begin(), fingerprints.end());
  std::string joined;
  for (auto& f : fingerprints) { joined += f; joined += '\n'; }
  summary.checksum_hex = HexEncode(crypto::Sha256(joined));
  return summary;
}

std::string NodeEngine::SignLedgerTip() const {
  WriteAheadLog::LedgerTip tip = storage_->LedgerTip();
  std::string message = std::to_string(tip.entry_id) + ":" + tip.entry_hash;
  return identity_->Sign(message);
}

}  // namespace desentry
