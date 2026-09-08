#include "desentry/engine/node_engine.h"

#include <algorithm>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/document_codec.h"

namespace desentry {

StatusOr<std::unique_ptr<NodeEngine>> NodeEngine::Open(const Options& options) {
  std::unique_ptr<NodeEngine> engine(new NodeEngine());
  engine->options_ = options;

  // The data directory has to exist before identity.key can be written into
  // it, and identity.key has to exist before anything else can be stamped
  // with this node's id -- so this is the one ordering constraint at boot.
  if (!MakeDirs(options.data_dir)) {
    return Status::IOError("cannot create data directory: " + options.data_dir);
  }
  auto id_or = NodeIdentity::LoadOrCreate(options.data_dir + "/identity.key");
  if (!id_or.ok()) return id_or.status();
  engine->identity_ = std::make_unique<NodeIdentity>(id_or.value());
  engine->clock_ = std::make_unique<HybridLogicalClock>(engine->identity_->node_id());

  StorageEngine::Options storage_opts;
  storage_opts.data_dir = options.data_dir;
  storage_opts.buffer_pool_pages = options.buffer_pool_pages;
  storage_opts.quota_mb = options.quota_mb;
  storage_opts.db_share_pct = options.db_share_pct;
  storage_opts.engines = options.engines;
  storage_opts.default_engine = options.default_engine;
  storage_opts.node_id = engine->identity_->node_id();
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
                                         engine->identity_->node_id());
  if (!transit_or.ok()) return transit_or.status();
  engine->transit_ = std::move(transit_or.value());

  engine->changes_ = std::make_unique<ChangeFeed>(engine->storage_->wal());
  ChangeFeed* feed = engine->changes_.get();
  engine->storage_->SetTipObserver([feed](lsn_t tip) { feed->Publish(tip); });

  DSN_LOG_INFO("engine", "node engine ready, node_id=" << engine->identity_->node_id()
                                                        << (options.supervisor ? " (supervisor)" : ""));
  return engine;
}

NodeEngine::~NodeEngine() {
  // Wake any in-flight long poll before the ledger goes away underneath it.
  if (changes_) changes_->Stop();
  if (storage_) storage_->SetTipObserver(nullptr);
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
  return WriteThrough(collection, key, EncodeDocument(updated), /*notify_hook=*/true);
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

  CrdtValue remote;
  try {
    remote = DecodeDocument(remote_encoded_doc);
  } catch (const std::exception& e) {
    return Status::Corruption(std::string("undecodable remote document: ") + e.what());
  }
  // Causality: our next Now() is guaranteed to be after this write, even if
  // the peer's wall clock is ahead of ours.
  clock_->Observe(remote.MaxTimestamp());

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
  return WriteThrough(collection, key, EncodeDocument(merged), /*notify_hook=*/false);
}

std::vector<DigestEntryOut> NodeEngine::LocalDigest(const std::string& collection) {
  std::vector<DigestEntryOut> out;
  auto raw = storage_->Scan(collection, "", 0);
  out.reserve(raw.size());
  for (auto& [key, bytes] : raw) {
    // Eight bytes is plenty: this only has to distinguish two copies of the
    // same key on two peers, and a collision costs a skipped exchange that
    // the next write repairs -- not a wrong merge.
    out.push_back(DigestEntryOut{key, DecodeDocument(bytes).MaxTimestamp(),
                                 crypto::Sha256(bytes).substr(0, 8)});
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
                                        const std::string& key, const std::string& encoded_doc) {
  if (owner_node.empty() || owner_node == identity_->node_id()) {
    return Status::InvalidArgument("transit: an owner must be another node");
  }

  TransitEnvelope envelope;
  envelope.owner_node = owner_node;
  envelope.collection = collection;
  envelope.key = key;
  envelope.key_hash = LedgerKeyHash(collection, key);
  envelope.encoded_doc = encoded_doc;
  envelope.holder_node = identity_->node_id();

  // Ledger first, bytes second. If the process dies between the two, the
  // intent names bytes that are missing -- which a returning owner discovers
  // and reports. The reverse order would leave bytes nothing points at,
  // which nothing would ever discover.
  const HLCTimestamp ts = clock_->Now();
  auto lsn_or = storage_->AppendLedgerOp(WalRecordType::kTransitIntent, owner_node, key,
                                          envelope.key_hash, ts);
  if (!lsn_or.ok()) return lsn_or.status();
  envelope.intent_lsn = lsn_or.value();

  Status st = transit_->Hold(envelope);
  if (!st.ok()) return st;
  DSN_LOG_INFO("transit", "holding " << collection << "/" << key << " for offline owner "
                                      << owner_node << " (intent LSN " << envelope.intent_lsn << ")");
  return Status::OK();
}

std::vector<TransitEnvelope> NodeEngine::PendingTransitFor(const std::string& owner_node) {
  return transit_->PendingFor(owner_node);
}

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
  return std::to_string(entry_id) + ":" + entry_hash;
}

std::string NodeEngine::SignLedgerTip() const {
  const WriteAheadLog::LedgerTip tip = storage_->LedgerTip();
  return identity_->Sign(LedgerTipMessage(tip.entry_id, tip.entry_hash));
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

}  // namespace desentry
