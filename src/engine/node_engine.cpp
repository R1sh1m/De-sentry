#include "desentry/engine/node_engine.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/ledger/outbox_store.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"
#include "desentry/storage/engines/ts_rollup.h"

namespace desentry {

StatusOr<std::unique_ptr<NodeEngine>> NodeEngine::Open(const Options& options) {
  std::unique_ptr<NodeEngine> engine(new NodeEngine());
  engine->options_ = options;
  engine->at_rest_sealed_ = !options.dek.empty();

  // The data directory has to exist before identity.key can be written into
  // it, and identity.key has to exist before anything else can be stamped
  // with this node's id -- so this is the one ordering constraint at boot.
  if (!MakeDirs(options.data_dir)) {
    return Status::IOError("cannot create data directory: " + options.data_dir);
  }
  if (!options.dek.empty() && options.dek.size() != 32) {
    return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
  }
  auto id_or = NodeIdentity::LoadOrCreate(options.data_dir + "/identity.key", options.dek);
  if (!id_or.ok()) return id_or.status();
  engine->identity_ = std::make_unique<NodeIdentity>(id_or.value());
  engine->clock_ = std::make_unique<HybridLogicalClock>(engine->identity_->node_id());
  engine->LoadClockState();

  StorageEngine::Options storage_opts;
  storage_opts.data_dir = options.data_dir;
  storage_opts.buffer_pool_pages = options.buffer_pool_pages;
  storage_opts.quota_mb = options.quota_mb;
  storage_opts.db_share_pct = options.db_share_pct;
  storage_opts.engines = options.engines;
  storage_opts.default_engine = options.default_engine;
  storage_opts.node_id = engine->identity_->node_id();
  storage_opts.dek = options.dek;
  auto storage_or = StorageEngine::Open(storage_opts);
  if (!storage_or.ok()) return storage_or.status();
  engine->storage_ = std::move(storage_or.value());

  // Install the signing callback so every subsequent ledger entry carries an
  // origin attestation. Entries written before this point (there are none in
  // normal operation, but recovery of a v1 log produces some) stay unsigned
  // and are reported as such rather than being treated as attested.
  NodeIdentity* identity = engine->identity_.get();
  engine->storage_->SetLedgerOrigin(identity->node_id(), [identity](const std::string& message) {
    return identity->Sign(message);
  });

  auto transit_or = TransitStore::Open(options.data_dir, options.transit_ttl_seconds,
                                         engine->identity_->node_id(), options.dek);
  if (!transit_or.ok()) return transit_or.status();
  engine->transit_ = std::move(transit_or.value());

  auto outbox_or =
      OutboxStore::Open(options.data_dir, engine->identity_->node_id(), options.dek);
  if (!outbox_or.ok()) return outbox_or.status();
  engine->outbox_ = std::move(outbox_or.value());

  engine->changes_ = std::make_unique<ChangeFeed>(engine->storage_->wal());
  ChangeFeed* feed = engine->changes_.get();
  engine->storage_->SetTipObserver([feed](lsn_t tip) { feed->Publish(tip); });

  engine->receipt_tracker_ = std::make_unique<ReceiptTracker>();

  // The DEK has been derived into per-file subkeys everywhere it is needed;
  // drop the raw key from the retained options so memory holds subkeys only.
  // (The sealed flag above preserves what /_status reports.)
  at_rest::Zeroize(engine->options_.dek);
  engine->options_.dek.clear();

  DSN_LOG_INFO("engine", "node engine ready, node_id=" << engine->identity_->node_id()
                                                        << (options.supervisor ? " (supervisor)" : "")
                                                        << (engine->at_rest_sealed_ ? " [sealed]" : ""));
  return engine;
}

NodeEngine::~NodeEngine() {
  // Wake any in-flight long poll before the ledger goes away underneath it.
  if (changes_) changes_->Stop();
  if (storage_) storage_->SetTipObserver(nullptr);
}

void NodeEngine::LoadClockState() {
  // 20-byte LE file: u64 physical_ms + u32 logical + u64 tag counter.
  // Missing/short file = first boot or pre-tag-persistence: the fields
  // present still apply (clock starts at 0, WAL stamps still order). Accepts
  // both the 12-byte (HLC only) and 20-byte shapes.
  const std::string path = options_.data_dir + "/hlc_clock";
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return;
  char buf[20];
  in.read(buf, 20);
  const std::streamsize got = in.gcount();
  if (got < 12) return;
  uint64_t phys = 0;
  uint32_t log = 0;
  std::memcpy(&phys, buf, 8);
  std::memcpy(&log, buf + 8, 4);
  clock_->Restore(phys, log);
  hlc_saved_physical_ = phys;
  if (got >= 20) {
    uint64_t tags = 0;
    std::memcpy(&tags, buf + 12, 8);
    CrdtValue::RestoreTagCounter(tags);
  }
}

void NodeEngine::PersistClockIfDue() {
  // Checkpoint at most ~1/s of wall advancement or every 512 ticks: crash
  // recovery replays at most a second of HLC space, never reused stamps.
  const uint64_t phys = clock_->last_physical();
  if (phys < hlc_saved_physical_ + 1000 && (++hlc_save_counter_ % 512) != 0) return;
  const uint32_t log = clock_->last_logical();
  const uint64_t tags = CrdtValue::TagCounter();
  const std::string path = options_.data_dir + "/hlc_clock";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    char buf[20];
    std::memcpy(buf, &phys, 8);
    std::memcpy(buf + 8, &log, 4);
    std::memcpy(buf + 12, &tags, 8);
    out.write(buf, 20);
    out.flush();
    if (!out.good()) return;
  }
  std::string sync_err;
  SyncFileByPath(tmp, &sync_err);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) return;
  SyncDirForFile(path, &sync_err);
  hlc_saved_physical_ = phys;
}

// ---------------------------------------------------------------------------
// Access control
// ---------------------------------------------------------------------------

bool NodeEngine::CanRead(const std::string& collection, const Requestor& who) const {
  // The node always reads its own store: a node that could lock itself out
  // of its own data by mis-setting an ACL would be a support nightmare, and
  // it gains an attacker nothing (they would already be on the machine).
  if (who.is_local && who.node_id == identity_->node_id()) return true;
  return storage_->catalog().CanRead(collection, who.node_id);
}

bool NodeEngine::CanWrite(const std::string& collection, const Requestor& who) const {
  if (who.is_local && who.node_id == identity_->node_id()) return true;
  return storage_->catalog().CanWrite(collection, who.node_id);
}

std::vector<std::string> NodeEngine::ReadableCollections(const Requestor& who) {
  std::vector<std::string> out;
  for (const std::string& name : storage_->ListCollections()) {
    // The transit collection is engine-internal: its envelopes are already
    // access-controlled by owner, and exposing it as an ordinary collection
    // would leak the fact that bytes are being held and for whom.
    if (name == kTransitCollection) continue;
    if (CanRead(name, who)) out.push_back(name);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

Status NodeEngine::WriteThrough(const std::string& collection, const std::string& key,
                                 const std::string& encoded_doc, bool notify_hook) {
  Status st = storage_->PutRaw(collection, key, encoded_doc);
  if (!st.ok()) return st;
  if (notify_hook && on_local_write_) on_local_write_(collection, key, encoded_doc);
  return Status::OK();
}

Status NodeEngine::PutDocument(const std::string& collection, const std::string& key,
                                 const JsonValue& new_json, const Requestor& who) {
  if (collection == kTransitCollection) {
    return Status::InvalidArgument("'" + std::string(kTransitCollection) +
                                    "' is engine-internal and cannot be written directly");
  }
  if (!CanWrite(collection, who)) {
    return Status::AuthError("node " + who.node_id + " may not write to collection " + collection);
  }

  auto existing_or = storage_->GetRaw(collection, key);
  CrdtValue previous;
  bool had_previous = false;
  if (existing_or.ok()) {
    previous = DecodeDocument(existing_or.value());
    had_previous = true;
  } else if (existing_or.status().code() != StatusCode::kNotFound) {
    return existing_or.status();
  }

  const HLCTimestamp ts = clock_->Now();
  CrdtValue updated = had_previous ? CrdtValue::ApplyJsonUpdate(previous, new_json, ts)
                                    : CrdtValue::FromJson(new_json, ts);
  const std::string encoded_doc = EncodeDocument(updated);

  // Always write locally first -- the local node must have the data regardless
  // of mesh connectivity.
  Status st = storage_->PutRaw(collection, key, encoded_doc);
  if (!st.ok()) return st;

  // Determine if we should broadcast now or stage for later.
  bool isolated = false;
  if (reachability_provider_) {
    isolated = !reachability_provider_();
  }

  if (isolated) {
    // Stage in outbox for replay on reconnect.
    OutboxEntry entry;
    entry.collection = collection;
    entry.key = key;
    entry.key_hash = LedgerKeyHash(collection, key);
    entry.encoded_doc = encoded_doc;
    entry.hlc = ts;
    Status outbox_st = outbox_->Put(entry);
    if (!outbox_st.ok()) {
      DSN_LOG_WARN("outbox", "failed to stage write in outbox: " << outbox_st.message());
    }
    DSN_LOG_INFO("outbox", "staged write for " << collection << "/" << key
                                               << " (node isolated, " << outbox_->Size()
                                               << " entries in outbox)");
  } else {
    // Normal path: notify the hook so the network layer broadcasts.
    if (on_local_write_) on_local_write_(collection, key, encoded_doc);
  }

  NoteDigest(collection, key, encoded_doc);
  PersistClockIfDue();
  return Status::OK();
}

Status NodeEngine::DeleteDocument(const std::string& collection, const std::string& key,
                                   const Requestor& who) {
  // A delete is a tombstoning write, not a physical removal -- that is what
  // makes it converge with a concurrent update on another node instead of
  // racing it.
  return PutDocument(collection, key, JsonValue::MakeObject(), who);
}

StatusOr<JsonValue> NodeEngine::GetDocument(const std::string& collection, const std::string& key,
                                             const Requestor& who) {
  if (!CanRead(collection, who)) {
    // Deliberately kNotFound, not kAuthError: an error that distinguishes
    // "you may not read this" from "this does not exist" tells a stranger
    // the collection is there. ListDocuments() and ReadableCollections()
    // already answer "nothing here" to non-readers; GetDocument() must not
    // be the oracle that contradicts them.
    return Status::NotFound("no such document: " + collection + "/" + key);
  }
  auto raw_or = storage_->GetRaw(collection, key);
  if (!raw_or.ok()) return raw_or.status();
  CrdtValue doc = DecodeDocument(raw_or.value());
  if (doc.IsEmpty()) return Status::NotFound("document deleted: " + key);
  return doc.ToJson();
}

std::vector<std::pair<std::string, JsonValue>> NodeEngine::ListDocuments(
    const std::string& collection, const std::string& start_key, size_t limit, const Requestor& who) {
  std::vector<std::pair<std::string, JsonValue>> out;
  if (!CanRead(collection, who)) return out;
  // Over-fetch to account for tombstoned (deleted-but-still-indexed)
  // documents filtered out below, so a caller asking for `limit` live
  // documents does not get a short page just because some keys in that range
  // are deleted.
  const size_t fetch_limit = limit == 0 ? 0 : limit * 2 + 16;
  for (auto& [key, bytes] : storage_->Scan(collection, start_key, fetch_limit)) {
    CrdtValue doc = DecodeDocument(bytes);
    if (doc.IsEmpty()) continue;
    out.emplace_back(key, doc.ToJson());
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

Status NodeEngine::MergeRemote(const std::string& collection, const std::string& key,
                                const std::string& remote_encoded_doc, const Requestor& who) {
  if (collection == kTransitCollection) {
    // Transit envelopes replicate between holders through the sidecar log,
    // not through document storage -- but only from an authenticated peer,
    // never from a local API caller pretending to be one.
    if (who.is_local) return Status::InvalidArgument("transit envelopes are not locally writable");
    auto env_or = TransitStore::Decode(remote_encoded_doc);
    if (!env_or.ok()) return env_or.status();
    return transit_->MergeRemoteEnvelope(env_or.value());
  }
  if (!CanWrite(collection, who)) {
    return Status::AuthError("node " + who.node_id + " may not write to collection " + collection);
  }
  // H-1 note: unknown collections merge under the local default here; the
  // confidentiality story is carried by replicated kAcl records (see
  // SetCollectionAcl/ApplyRemoteAcl), not by refusing merges -- refusing
  // them broke convergence outright (every multi-writer collection must be
  // pre-provisioned on every node). A merge never *serves* bytes; serving
  // stays gated on the effective ACL.

  CrdtValue remote;
  try {
    remote = DecodeDocument(remote_encoded_doc);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("undecodable remote document: ") + e.what());
  }
  // Causality: our next Now() is guaranteed to be after this write, even if
  // the peer's wall clock is ahead of ours. A remote stamp beyond the skew
  // bound is rejected outright (C-5): merging it would pin the clock and let
  // one write become permanently unoverwritable.
  if (!clock_->Observe(remote.MaxTimestamp())) {
    return Status::InvalidArgument("remote timestamp too far in the future; rejected");
  }

  auto existing_or = storage_->GetRaw(collection, key);
  CrdtValue merged;
  if (existing_or.ok()) {
    merged = CrdtValue::Merge(DecodeDocument(existing_or.value()), remote);
  } else if (existing_or.status().code() == StatusCode::kNotFound) {
    merged = remote;
  } else {
    return existing_or.status();
  }

  // Deliberately does not invoke on_local_write_. Relaying every merged
  // remote write back out from here would rebroadcast it to the peer we just
  // received it from. v2 does relay -- but in NetworkManager, where the
  // message-id dedup cache and the TTL live, so a relay is bounded and
  // loop-free by construction rather than by luck.
  const std::string merged_bytes = EncodeDocument(merged);
  Status wst = WriteThrough(collection, key, merged_bytes, /*notify_hook=*/false);
  if (wst.ok()) {
    NoteDigest(collection, key, merged_bytes);
    PersistClockIfDue();
  }
  return wst;
}

std::string NodeEngine::DigestCacheKey(const std::string& collection, const std::string& key) {
  std::string k = collection;
  k.push_back('\0');
  k += key;
  return k;
}

void NodeEngine::NoteDigest(const std::string& collection, const std::string& key,
                            const std::string& encoded_doc) {
  DigestEntryOut entry;
  entry.key = key;
  try {
    entry.top_ts = DecodeDocument(encoded_doc).MaxTimestamp();
  } catch (const std::exception&) {
    return;  // never cache a failure; the digest path recomputes
  }
  // Eight bytes is plenty: this only has to distinguish two copies of the
  // same key on two peers, and a collision costs a skipped exchange that
  // the next write repairs -- not a wrong merge.
  entry.content_hash = crypto::Sha256(encoded_doc).substr(0, 8);
  std::lock_guard<std::mutex> lock(digest_mu_);
  digest_cache_[DigestCacheKey(collection, key)] = std::move(entry);
}

std::vector<DigestEntryOut> NodeEngine::LocalDigest(const std::string& collection) {
  std::vector<DigestEntryOut> out;
  auto raw = storage_->Scan(collection, "", 0);
  out.reserve(raw.size());
  std::lock_guard<std::mutex> lock(digest_mu_);
  for (auto& [key, bytes] : raw) {
    auto it = digest_cache_.find(DigestCacheKey(collection, key));
    if (it != digest_cache_.end()) {
      out.push_back(it->second);
      continue;
    }
    DigestEntryOut entry;
    entry.key = key;
    try {
      entry.top_ts = DecodeDocument(bytes).MaxTimestamp();
    } catch (const std::exception&) {
      continue;  // undecodable row: digest skips it (merge path rejects too)
    }
    entry.content_hash = crypto::Sha256(bytes).substr(0, 8);
    digest_cache_[DigestCacheKey(collection, key)] = entry;
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<std::string> NodeEngine::ListCollections() {
  std::vector<std::string> out;
  for (const std::string& name : storage_->ListCollections()) {
    if (name == kTransitCollection) continue;
    out.push_back(name);
  }
  return out;
}

StatusOr<std::string> NodeEngine::GetRawEncoded(const std::string& collection,
                                                  const std::string& key) {
  return storage_->GetRaw(collection, key);
}

CollectionSummary NodeEngine::Summarize(const std::string& collection) {
  CollectionSummary summary;
  summary.name = collection;
  summary.engine = storage_->router().EngineNameFor(collection);
  summary.is_private = storage_->catalog().EffectiveAcl(collection).is_private;
  // The checksum comes from the backend so it is computed the same way
  // whichever layout the collection lives in -- see storage/router.h's second
  // invariant. The document count is recomputed here because a backend's
  // checksum deliberately skips tombstones and we want both numbers from one
  // pass over the same rows.
  auto rows = storage_->Scan(collection, "", 0);
  for (auto& [key, bytes] : rows) {
    (void)key;
    if (DecodeDocument(bytes).IsEmpty()) continue;
    ++summary.document_count;
  }
  summary.checksum_hex = storage_->router().Checksum(collection);
  return summary;
}

// ---------------------------------------------------------------------------
// Transit
// ---------------------------------------------------------------------------

Status NodeEngine::HoldForOfflineOwner(const std::string& owner_node, const std::string& collection,
                                        const std::string& key, const std::string& encoded_doc,
                                        const std::vector<uint32_t>* only_chunks,
                                        const std::string& message_id) {
  if (owner_node.empty() || owner_node == identity_->node_id()) {
    return Status::InvalidArgument("transit: an owner must be another node");
  }
  const size_t chunk_size =
      options_.transit_chunk_bytes > 0 ? options_.transit_chunk_bytes : 256 * 1024;
  // Shared with the network layer's holder selection (TransitChunkCount):
  // both sides must agree on chunk indexes for the same document.
  const uint32_t chunk_total = TransitChunkCount(encoded_doc.size(), options_.transit_chunk_bytes);
  const std::string doc_key_hash = LedgerKeyHash(collection, key);

  // Select the chunks this call holds, validating the assignment: an index
  // past the end is a caller bug, not a partial hold.
  std::vector<uint32_t> indexes;
  if (only_chunks != nullptr) {
    for (uint32_t i : *only_chunks) {
      if (i >= chunk_total) {
        return Status::InvalidArgument("transit: chunk index out of range");
      }
      indexes.push_back(i);
    }
  } else {
    for (uint32_t i = 0; i < chunk_total; ++i) indexes.push_back(i);
  }

  // Capacity gate, all-or-nothing: the bytes this call would add are checked
  // against the transit budget before the first chunk is stored, so a full
  // holder refuses the whole document instead of stranding a partial hold
  // whose unclaimed intents would pin the checkpoint gate. A zero budget is
  // unbounded (quota_mb == 0) and always fits.
  uint64_t new_bytes = 0;
  for (uint32_t i : indexes) {
    const size_t off = static_cast<size_t>(i) * chunk_size;
    new_bytes += std::min(chunk_size, encoded_doc.size() - off);
  }
  if (options_.transit_budget_bytes > 0 &&
      transit_->BytesHeld() + new_bytes > options_.transit_budget_bytes) {
    return Status::OutOfSpace("transit: holder budget exhausted (" +
                              std::to_string(transit_->BytesHeld()) + " held, " +
                              std::to_string(new_bytes) + " requested)");
  }

  for (uint32_t i : indexes) {
    const size_t off = static_cast<size_t>(i) * chunk_size;
    const size_t len = std::min(chunk_size, encoded_doc.size() - off);
    TransitEnvelope envelope;
    envelope.owner_node = owner_node;
    envelope.collection = collection;
    envelope.key = key;
    envelope.key_hash =
        chunk_total == 1 ? doc_key_hash : TransitChunkKeyHash(doc_key_hash, i);
    envelope.encoded_doc = encoded_doc.substr(off, len);
    envelope.holder_node = identity_->node_id();
    envelope.doc_size_bytes = encoded_doc.size();
    envelope.chunk_index = i;
    envelope.chunk_total = chunk_total;
    envelope.message_id = message_id;
    // Content attestation (C-3 fix): hash this chunk's bytes and sign the
    // domain-separated attestation so the owner can verify served bytes.
    envelope.content_hash = crypto::Sha256(envelope.encoded_doc);
    envelope.holder_sig = identity_->Sign(
        TransitAttestMessage(envelope.key_hash, envelope.content_hash, i, chunk_total));

    // Ledger first, bytes second. If the process dies between the two, the
    // intent names bytes that are missing -- which a returning owner discovers
    // and reports. The reverse order would leave bytes nothing points at,
    // which nothing would ever discover.
    const HLCTimestamp ts = clock_->Now();
    WriteAheadLog::AppendOptions intent_opts;
    intent_opts.transit_holder = identity_->node_id();
    intent_opts.transit_size_bytes = encoded_doc.size();
    intent_opts.transit_chunk_index = i;
    intent_opts.transit_chunk_total = chunk_total;
    auto lsn_or = storage_->AppendLedgerOp(WalRecordType::kTransitIntent, owner_node, key,
                                            envelope.key_hash, ts, intent_opts);
    if (!lsn_or.ok()) return lsn_or.status();
    envelope.intent_lsn = lsn_or.value();

    Status st = transit_->Hold(envelope);
    if (!st.ok()) return st;
    DSN_LOG_INFO("transit", "holding " << collection << "/" << key << " chunk " << i << "/"
                                        << chunk_total << " for offline owner " << owner_node
                                        << " (intent LSN " << envelope.intent_lsn << ")");
  }
  return Status::OK();
}

std::vector<TransitEnvelope> NodeEngine::PendingTransitFor(const std::string& owner_node) {
  return transit_->PendingFor(owner_node);
}

bool NodeEngine::TransitLoadCorrupt() const { return transit_->LoadCorrupt(); }

Status NodeEngine::ApplyClaimedTransit(const std::string& collection, const std::string& key,
                                        const std::string& encoded_doc) {
  // Applying held bytes is an ordinary CRDT merge -- the document may well
  // have been written locally too while this node was offline, and the merge
  // is what reconciles the two.
  Status st = MergeRemote(collection, key, encoded_doc, Requestor::Peer(identity_->node_id()));
  if (!st.ok()) return st;
  const HLCTimestamp ts = clock_->Now();
  // The claim is recorded under this node's own id as the collection, so its
  // key_hash -- SHA-256(self || key) -- is the same value the holder's
  // TRANSIT_INTENT for these bytes carries (the holder records the owner in
  // its collection field too). Recording it under the document's collection
  // instead would hash differently and the pair would never match, leaving
  // every intent permanently "unclaimed" as far as the checkpoint gate can
  // tell.
  auto lsn_or = storage_->AppendLedgerOp(WalRecordType::kTransitClaimed, identity_->node_id(), key,
                                           LedgerKeyHash(collection, key), ts);
  if (!lsn_or.ok()) return lsn_or.status();
  DSN_LOG_INFO("transit", "claimed held document " << collection << "/" << key);
  PersistClockIfDue();
  return Status::OK();
}

Status NodeEngine::RecordRemoteClaim(const std::string& owner_node, const std::string& key_hash) {
  if (key_hash.size() != kWalHashLen) {
    return Status::InvalidArgument("transit claim: key_hash is not a SHA-256 digest");
  }
  const HLCTimestamp ts = clock_->Now();
  // The owner is recorded in the collection field, matching the INTENT this
  // claim settles -- RunCheckpoint() reads it back from there to release the
  // envelope. The key must be the real key, recovered from the envelope, so
  // the stored key_hash -- SHA-256(owner || key) -- equals the intent's. A
  // hex-of-hash placeholder would hash differently and never match.
  std::string key = HexEncode(key_hash);
  auto env_or = transit_->Lookup(owner_node, key_hash);
  if (env_or.ok()) {
    key = env_or.value().key;
  } else {
    // Best effort: the envelope is already gone (expired or released), so
    // there is nothing left to match against. The claim is still recorded --
    // the ledger is the audit trail, and a missing envelope is itself
    // information -- but it settles no pair.
    DSN_LOG_WARN("transit", "claim from " << owner_node << " names bytes this node no longer holds");
  }
  auto lsn_or = storage_->AppendLedgerOp(WalRecordType::kTransitClaimed, owner_node, key, key_hash,
                                          ts);
  if (!lsn_or.ok()) return lsn_or.status();
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

std::string NodeEngine::LedgerTipMessage(lsn_t entry_id, const std::string& entry_hash) {
  // Domain-separated (C-6/M-7 fix): the old "<id>:<hash>" string was also a
  // plausible signature message in other contexts. Binds node identity via
  // the signer, not the string, so VerifyPeerTip still checks DeriveNodeId.
  return "DSN-TIP-v1:" + std::to_string(entry_id) + ":" + entry_hash;
}

std::string NodeEngine::SignLedgerTip() const {
  const WriteAheadLog::LedgerTip tip = storage_->LedgerTip();
  return SignTipFor(tip.entry_id, tip.entry_hash);
}

std::string NodeEngine::SignTipFor(lsn_t entry_id, const std::string& entry_hash) const {
  return identity_->Sign(LedgerTipMessage(entry_id, entry_hash));
}

bool NodeEngine::VerifyPeerTip(const std::string& node_id, lsn_t entry_id,
                                const std::string& entry_hash, const std::string& signature) const {
  if (!resolve_public_key_) return false;
  const std::string public_key = resolve_public_key_(node_id);
  if (public_key.empty()) return false;
  // The node_id is itself SHA-256 of the public key, so checking that the
  // key we were handed actually derives the id we were told closes the gap
  // where a peer supplies someone else's key alongside a matching signature.
  if (NodeIdentity::DeriveNodeId(public_key) != node_id) return false;
  return NodeIdentity::Verify(public_key, LedgerTipMessage(entry_id, entry_hash), signature);
}

WriteAheadLog::VerifyResult NodeEngine::VerifyLedger() {
  auto resolver = resolve_public_key_;
  WriteAheadLog::SignatureVerifier verifier;
  if (resolver) {
    const std::string self_id = identity_->node_id();
    const std::string self_key = identity_->public_key();
    verifier = [resolver, self_id, self_key](const std::string& node_id, const std::string& message,
                                              const std::string& signature) {
      const std::string public_key = node_id == self_id ? self_key : resolver(node_id);
      if (public_key.empty()) return false;
      if (NodeIdentity::DeriveNodeId(public_key) != node_id) return false;
      return NodeIdentity::Verify(public_key, message, signature);
    };
  }
  return storage_->VerifyLedger(verifier);
}

StorageEngine::VerifyReport NodeEngine::VerifyEverything() {
  StorageEngine::VerifyReport report;
  report.ledger = VerifyLedger();
  Status backends = storage_->router().Verify();
  report.backends_ok = backends.ok();
  if (!backends.ok()) report.backend_failure = backends.message();
  return report;
}

std::vector<NodeEngine::TransitIntent> NodeEngine::TransitIntentsForSelf() {
  const std::string self = identity_->node_id();
  std::vector<TransitIntent> intents;

  // Read from the beginning up to the current tip. The ledger is append-only
  // so this is O(log size) in practice (one read of the full log). For a
  // returning node this is a one-time cost; the intents are then used to
  // target specific holders instead of polling everyone.
  auto entries_or = storage_->LedgerEntries(kInvalidLsn, storage_->LedgerTip().entry_id);
  if (!entries_or.ok()) return intents;

  for (const WalRecord& rec : entries_or.value()) {
    if (rec.type != WalRecordType::kTransitIntent) continue;
    // Transit intents record the owner in the collection field.
    if (rec.collection != self) continue;

    TransitIntent intent;
    intent.intent_lsn = rec.lsn;
    intent.key_hash = rec.key_hash;
    intent.holder_node = rec.transit_holder;
    intent.chunk_index = rec.transit_chunk_index;
    intent.chunk_total = rec.transit_chunk_total;
    intent.doc_size_bytes = rec.transit_size_bytes;
    intents.push_back(std::move(intent));
  }
  return intents;
}

// ---------------------------------------------------------------------------
// Replicated ACLs (H-1)
// ---------------------------------------------------------------------------

namespace {

// Canonical ACL JSON shared by the attestation signer and verifier: field
// order and reader order are fixed so both sides sign the same bytes.
std::string CanonicalAclJson(const CollectionAcl& acl, uint64_t updated_ms) {
  std::vector<std::string> readers = acl.readers;
  std::sort(readers.begin(), readers.end());
  JsonValue::Object o;
  o.emplace_back("owner_node", JsonValue(acl.owner_node));
  o.emplace_back("private", JsonValue(acl.is_private));
  JsonValue::Array r;
  for (const std::string& reader : readers) r.emplace_back(JsonValue(reader));
  o.emplace_back("readers", JsonValue(std::move(r)));
  o.emplace_back("parent", JsonValue(acl.parent));
  o.emplace_back("updated_ms", JsonValue(static_cast<int64_t>(updated_ms)));
  return JsonValue(std::move(o)).Dump();
}

std::string AclAttestMessage(const std::string& collection, const std::string& canonical_acl) {
  return "DSN-ACL-v1" + collection + '\0' + canonical_acl;
}

}  // namespace

Status NodeEngine::SetCollectionAcl(const std::string& collection, const CollectionAcl& acl) {
  Status ens_st = storage_->EnsureCollection(collection);
  if (!ens_st.ok()) return ens_st;
  const uint64_t ms = static_cast<uint64_t>(NowMs());
  Status acl_st = storage_->catalog().SetAclAt(collection, acl, ms);
  if (!acl_st.ok()) return acl_st;
  // Attest as the owner so peers can verify (origin == owner rule). The
  // ledger's own per-entry signature covers the envelope too; this inner
  // attestation is what a peer verifies without the full entry content.
  const std::string canonical = CanonicalAclJson(acl, ms);
  JsonValue::Object env;
  env.emplace_back("acl", JsonValue::Parse(canonical));
  env.emplace_back("updated_ms", JsonValue(static_cast<int64_t>(ms)));
  env.emplace_back("sig", JsonValue(identity_->Sign(AclAttestMessage(collection, canonical))));
  const HLCTimestamp ts = clock_->Now();
  auto lsn_or = storage_->AppendLedgerOp(WalRecordType::kAcl, collection, "",
                                         JsonValue(std::move(env)).Dump(), ts);
  if (!lsn_or.ok()) return lsn_or.status();
  PersistClockIfDue();
  return Status::OK();
}

Status NodeEngine::ApplyRemoteAcl(const std::string& origin_node_id, const std::string& collection,
                                  const std::string& envelope_json, bool check_sig) {
  JsonValue env;
  try {
    env = JsonValue::Parse(envelope_json);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("malformed kAcl envelope: ") + e.what());
  }
  if (!env.is_object()) return Status::Corruption("malformed kAcl envelope");
  const JsonValue* acl_v = env.Find("acl");
  const JsonValue* ms_v = env.Find("updated_ms");
  const JsonValue* sig_v = env.Find("sig");
  if (acl_v == nullptr || !acl_v->is_object() || ms_v == nullptr || !ms_v->is_number() ||
      sig_v == nullptr || !sig_v->is_string()) {
    return Status::Corruption("malformed kAcl envelope");
  }
  CollectionAcl acl;
  const JsonValue* owner = acl_v->Find("owner_node");
  if (owner && owner->is_string()) acl.owner_node = owner->AsString();
  const JsonValue* priv = acl_v->Find("private");
  if (priv && priv->is_bool()) acl.is_private = priv->AsBool();
  const JsonValue* readers = acl_v->Find("readers");
  if (readers && readers->is_array()) {
    for (const JsonValue& r : readers->AsArray()) {
      if (r.is_string()) acl.readers.push_back(r.AsString());
    }
  }
  const JsonValue* parent = acl_v->Find("parent");
  if (parent && parent->is_string()) acl.parent = parent->AsString();
  const uint64_t ms = static_cast<uint64_t>(ms_v->AsInt());
  if (check_sig) {
    // Self-attestation only: the origin must BE the claimed owner, with a
    // valid signature under its handshake-proven key. A peer cannot set,
    // clear, or override another node's ACL (M-style forgery rejected).
    if (acl.owner_node.empty() || acl.owner_node != origin_node_id) {
      return Status::AuthError("kAcl origin is not the claimed owner; rejected");
    }
    const uint64_t wall = static_cast<uint64_t>(NowMs());
    if (ms > wall + HybridLogicalClock::kMaxFutureSkewMs) {
      return Status::InvalidArgument("kAcl version too far in the future; rejected");
    }
    std::string public_key;
    if (origin_node_id == identity_->node_id()) {
      public_key = identity_->public_key();
    } else {
      if (!resolve_public_key_) return Status::AuthError("no key resolver; cannot verify kAcl");
      public_key = resolve_public_key_(origin_node_id);
    }
    if (public_key.empty() || NodeIdentity::DeriveNodeId(public_key) != origin_node_id) {
      return Status::AuthError("kAcl signer key unknown or mismatched; rejected");
    }
    const std::string canonical = CanonicalAclJson(acl, ms);
    if (!NodeIdentity::Verify(public_key, AclAttestMessage(collection, canonical), sig_v->AsString())) {
      return Status::AuthError("kAcl attestation does not verify; rejected");
    }
  }
  Status ens_st = storage_->EnsureCollection(collection);
  if (!ens_st.ok()) return ens_st;
  Status set_st = storage_->catalog().SetAclAt(collection, acl, ms);
  // A stale version racing a newer local ACL is not an error worth
  // propagating: last-writer-wins already picked the newer one.
  if (!set_st.ok() && set_st.code() == StatusCode::kInvalidArgument) return Status::OK();
  return set_st;
}

// ---------------------------------------------------------------------------
// Retention (explicit, operator-driven)
// ---------------------------------------------------------------------------

StatusOr<size_t> NodeEngine::RunRetention(const std::string& collection) {
  CollectionMeta meta;
  if (!storage_->catalog().GetCopy(collection, &meta)) {
    return Status::NotFound("no such collection: " + collection);
  }
  if (meta.retention_days == 0) {
    return Status::InvalidArgument("collection '" + collection +
                                   "' has no retention_days configured (PUT placement first)");
  }
  if (storage_->router().EngineNameFor(collection) != "ts_rollup") {
    return Status::InvalidArgument("retention is only honored by the ts_rollup engine; '" +
                                   collection + "' is on " +
                                   storage_->router().EngineNameFor(collection));
  }
  EngineBackend* backend = storage_->router().BackendFor(collection);
  auto* ts = dynamic_cast<TsRollupBackend*>(backend);
  if (ts == nullptr) return Status::Internal("ts_rollup backend unavailable for '" + collection + "'");
  auto dropped_or = ts->ApplyRetention(collection, meta.retention_days);
  if (!dropped_or.ok()) return dropped_or.status();
  DSN_LOG_INFO("retention", "collection '" << collection << "' dropped " << dropped_or.value()
                                           << " chunk(s) older than " << meta.retention_days
                                           << " day(s)");
  return dropped_or.value();
}

// ---------------------------------------------------------------------------
// Outbox (stage-anywhere, sync-on-reconnect flow)
// ---------------------------------------------------------------------------

size_t NodeEngine::FlushOutbox() {
  if (!outbox_) return 0;
  auto entries = outbox_->DrainAll();
  if (entries.empty()) return 0;
  size_t replayed = 0;
  for (const auto& entry : entries) {
    // Apply as a local write: this will go through CRDT merge and notify the
    // local write hook so it gets broadcast to peers.
    Status st = PutDocument(entry.collection, entry.key, DecodeDocument(entry.encoded_doc).ToJson(),
                            Requestor::Local(identity_->node_id()));
    if (st.ok()) {
      ++replayed;
    } else {
      DSN_LOG_WARN("outbox", "failed to replay outbox entry " << entry.collection << "/"
                                                             << entry.key << ": " << st.message());
      // Put it back in the outbox for retry
      outbox_->Put(entry);
    }
  }
  if (replayed > 0) {
    DSN_LOG_INFO("outbox", "replayed " << replayed << " out of " << entries.size()
                                       << " staged write(s) on reconnect");
  }
  return replayed;
}

size_t NodeEngine::OutboxSize() const {
  if (!outbox_) return 0;
  return outbox_->Size();
}

uint64_t NodeEngine::OutboxBytesHeld() const {
  if (!outbox_) return 0;
  return outbox_->BytesHeld();
}

StatusOr<NodeEngine::DurabilityReport> NodeEngine::WaitForDurability(
    const std::string& message_id, uint32_t durability, uint32_t timeout_ms) {
  // Count self as 1 (local write is already fsync'd and on the ledger).
  uint32_t wanted_from_peers = durability > 1 ? durability - 1 : 0;
  DurabilityReport report;
  report.requested = durability;
  report.achieved = 1;  // self
  report.replicas.push_back(identity_->node_id());

  if (wanted_from_peers == 0) {
    return report;  // local-only durability, nothing to wait for
  }

  if (!receipt_tracker_) {
    // No receipt tracker means we're running without network layer --
    // return what we have (just self).
    report.timed_out = true;
    return report;
  }

  auto result = receipt_tracker_->WaitFor(message_id, wanted_from_peers, timeout_ms);
  if (!result.ok()) {
    // Tracker stopped or other error -- return partial.
    report.timed_out = true;
    return report;
  }

  const auto& receipts = result.value();
  report.achieved = 1 + static_cast<uint32_t>(receipts.size());
  for (const MergeReceipt& r : receipts) {
    report.replicas.push_back(r.applier_node);
  }
  if (receipts.size() < wanted_from_peers) {
    report.timed_out = true;
  }
  return report;
}

}  // namespace desentry
