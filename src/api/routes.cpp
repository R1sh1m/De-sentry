#include "desentry/api/routes.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>

#include <openssl/evp.h>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/net/admission.h"
#include "desentry/security/crypto.h"
#include "desentry/storage/engines/graph_adj.h"
#include "desentry/storage/engines/ts_rollup.h"
#include "desentry/storage/engines/vector_hnsw_lite.h"
#include "desentry/storage/engines/vendored_backends.h"

namespace desentry {

namespace {

bool TypeMatches(JsonType actual, const std::string& expected) {
  if (expected == "string") return actual == JsonType::kString;
  if (expected == "number") return actual == JsonType::kInt || actual == JsonType::kDouble;
  if (expected == "integer") return actual == JsonType::kInt;
  if (expected == "boolean") return actual == JsonType::kBool;
  if (expected == "array") return actual == JsonType::kArray;
  if (expected == "object") return actual == JsonType::kObject;
  if (expected == "null") return actual == JsonType::kNull;
  return true;  // unknown expected-type token: don't block on it
}

HttpResponse JsonOk(const JsonValue& v, int status = 200) { return HttpResponse::Json(status, v.Dump()); }

HttpResponse JsonError(int status, const std::string& message) {
  JsonValue::Object obj;
  obj.emplace_back("error", JsonValue(message));
  return HttpResponse::Json(status, JsonValue(std::move(obj)).Dump());
}

int StatusToHttpCode(const Status& st) {
  switch (st.code()) {
    case StatusCode::kNotFound: return 404;
    case StatusCode::kAlreadyExists: return 409;
    case StatusCode::kInvalidArgument: return 400;
    case StatusCode::kSchemaViolation: return 422;
    case StatusCode::kAuthError: return 403;
    case StatusCode::kOutOfSpace: return 507;
    default: return 500;
  }
}

HttpResponse StatusError(const Status& st) { return JsonError(StatusToHttpCode(st), st.message()); }

HttpResponse Ok() {
  JsonValue::Object obj;
  obj.emplace_back("ok", JsonValue(true));
  return JsonOk(JsonValue(std::move(obj)));
}

int64_t QueryInt(const HttpRequest& req, const char* name, int64_t fallback) {
  auto it = req.query.find(name);
  if (it == req.query.end()) return fallback;
  try {
    return std::stoll(it->second);
  } catch (...) {
    return fallback;
  }
}

std::string QueryString(const HttpRequest& req, const char* name, const std::string& fallback = "") {
  auto it = req.query.find(name);
  return it == req.query.end() ? fallback : it->second;
}

JsonValue LedgerEntryToJson(const WalRecord& rec) {
  JsonValue::Object e;
  e.emplace_back("entry_id", JsonValue(static_cast<int64_t>(rec.lsn)));
  e.emplace_back("operation", JsonValue(WalRecordTypeName(rec.type)));
  e.emplace_back("collection", JsonValue(rec.collection));
  e.emplace_back("key", JsonValue(rec.key));
  e.emplace_back("key_hash", JsonValue(HexEncode(rec.key_hash)));
  e.emplace_back("document_bytes", JsonValue(static_cast<int64_t>(rec.document_bytes.size())));
  e.emplace_back("hlc", JsonValue(rec.hlc.ToString()));
  e.emplace_back("origin_node_id", JsonValue(rec.origin_node_id));
  e.emplace_back("origin_signature", JsonValue(HexEncode(rec.origin_signature)));
  e.emplace_back("prev_hash", JsonValue(HexEncode(rec.prev_hash)));
  e.emplace_back("entry_hash", JsonValue(HexEncode(rec.entry_hash)));
  return JsonValue(std::move(e));
}

JsonValue PeerToJson(const PeerInfo& p, lsn_t network_max, int64_t now_ms,
                     int64_t liveness_threshold_ms) {
  JsonValue::Object obj;
  obj.emplace_back("node_id", JsonValue(p.node_id));
  obj.emplace_back("host", JsonValue(p.host));
  obj.emplace_back("hostname", JsonValue(p.hostname));
  obj.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(p.p2p_port)));
  obj.emplace_back("api_port", JsonValue(static_cast<int64_t>(p.api_port)));
  obj.emplace_back("last_seen_ms", JsonValue(static_cast<int64_t>(p.last_seen_ms)));
  obj.emplace_back("supervisor", JsonValue(p.is_supervisor));
  obj.emplace_back("handshake_proven", JsonValue(p.handshake_proven));
  obj.emplace_back("state", JsonValue(NodeLifecycleStateName(p.state)));
  obj.emplace_back("suspicion",
                   JsonValue(PeerSuspicionName(p.Suspicion(now_ms, liveness_threshold_ms))));

  JsonValue::Object fitness;
  fitness.emplace_back("latency_ms", JsonValue(p.fitness.latency_ms));
  fitness.emplace_back("success_rate", JsonValue(p.fitness.success_rate));
  fitness.emplace_back("ledger_freshness_entry_id",
                       JsonValue(static_cast<int64_t>(p.fitness.ledger_freshness_entry_id)));
  fitness.emplace_back("free_quota_mb", JsonValue(static_cast<int64_t>(p.fitness.free_quota_mb)));
  fitness.emplace_back("probes", JsonValue(static_cast<int64_t>(p.fitness.probes)));
  fitness.emplace_back("score", JsonValue(p.fitness.Score(network_max)));
  obj.emplace_back("fitness", JsonValue(std::move(fitness)));
  return JsonValue(std::move(obj));
}

JsonValue AclToJson(const CollectionAcl& acl) {
  JsonValue::Object obj;
  obj.emplace_back("owner_node", JsonValue(acl.owner_node));
  obj.emplace_back("private", JsonValue(acl.is_private));
  JsonValue::Array readers;
  for (const std::string& r : acl.readers) readers.emplace_back(JsonValue(r));
  obj.emplace_back("readers", JsonValue(std::move(readers)));
  obj.emplace_back("parent", JsonValue(acl.parent));
  return JsonValue(std::move(obj));
}

// Decode base64 (padded, as produced by FileReader.readAsDataURL) to binary.
// Returns empty vector on failure.
std::vector<uint8_t> DecodeBase64(const std::string& input) {
  if (input.empty()) return {};
  if (input.size() % 4 != 0) return {};  // invalid padding

  std::vector<uint8_t> output((input.size() * 3) / 4);
  int decoded = EVP_DecodeBlock(output.data(),
                                 reinterpret_cast<const unsigned char*>(input.c_str()),
                                 static_cast<int>(input.size()));
  if (decoded < 0) return {};

  // Adjust for padding
  if (input.size() >= 2 && input[input.size() - 1] == '=') decoded--;
  if (input.size() >= 3 && input[input.size() - 2] == '=') decoded--;

  output.resize(static_cast<size_t>(decoded));
  return output;
}

// Reassemble a chunked file's base64 content from its parts.
StatusOr<std::string> ReassembleChunkedFile(NodeEngine* engine,
                                             const std::string& collection,
                                             const std::string& key,
                                             const JsonValue& manifest,
                                             const Requestor& self) {
  const JsonValue* v_ct = manifest.Find("chunk_total");
  const int64_t chunk_total = (v_ct && v_ct->is_number()) ? v_ct->AsInt() : 0;
  if (chunk_total <= 0) {
    return Status::InvalidArgument("chunked manifest missing chunk_total");
  }
  const JsonValue* v_cf = manifest.Find("chunk_field");
  const std::string chunk_field = (v_cf && v_cf->is_string()) ? v_cf->AsString() : "base64";

  std::string result;
  result.reserve(static_cast<size_t>(chunk_total) * 4000);  // rough estimate

  for (int64_t i = 0; i < chunk_total; ++i) {
    std::string part_key = key + "#" + std::to_string(i);
    auto part_or = engine->GetDocument(collection, part_key, self);
    if (!part_or.ok()) {
      return Status::NotFound("missing chunk " + std::to_string(i) + " for " + key);
    }
    const JsonValue& part = part_or.value();
    // Chunk records store the original field name in `field` and the
    // payload in `text`; `chunk_field` belongs to the manifest.
    const JsonValue* v_field = part.Find("field");
    if (!v_field || !v_field->is_string() || v_field->AsString() != chunk_field) {
      return Status::InvalidArgument("chunk " + std::to_string(i) + " has an unexpected field");
    }
    const JsonValue* v_cd = part.Find("text");
    const std::string chunk_data = (v_cd && v_cd->is_string()) ? v_cd->AsString() : "";
    if (chunk_data.empty()) {
      return Status::InvalidArgument("chunk " + std::to_string(i) + " missing text");
    }
    result += chunk_data;
  }
  return result;
}

}  // namespace

Status ValidateAgainstSchema(const JsonValue& doc, const JsonValue& schema) {
  if (!schema.is_object()) return Status::OK();

  const JsonValue* type = schema.Find("type");
  if (type && type->is_string() && type->AsString() == "object" && !doc.is_object()) {
    return Status::SchemaViolation("document must be a JSON object per collection schema");
  }

  const JsonValue* props = schema.Find("properties");
  if (props && props->is_object()) {
    for (auto& [field, spec] : props->AsObject()) {
      const JsonValue* val = doc.Find(field);
      if (val == nullptr) continue;  // presence is governed by "required", not "properties"
      const JsonValue* expected_type = spec.Find("type");
      if (expected_type && expected_type->is_string() && !TypeMatches(val->type(), expected_type->AsString())) {
        return Status::SchemaViolation("field '" + field + "' expected type " + expected_type->AsString());
      }
    }
  }

  const JsonValue* required = schema.Find("required");
  if (required && required->is_array()) {
    for (auto& name : required->AsArray()) {
      if (!name.is_string()) continue;
      if (!doc.Has(name.AsString())) {
        return Status::SchemaViolation("missing required field: " + name.AsString());
      }
    }
  }
  return Status::OK();
}

// Forward declaration for file routes
void RegisterFileRoutes(HttpServer* server, NodeEngine* engine, NetworkManager* network, std::function<Requestor()> self);

void RegisterRoutes(HttpServer* server, NodeEngine* engine, NetworkManager* network) {
  static auto start_time = std::chrono::steady_clock::now();

  // Every local API call is attributed to this node's own identity. The API
  // binds to loopback and the app is the only client, so "who is asking" is
  // never taken from a header a caller could set -- an ACL that could be
  // bypassed by claiming a different node_id would not be an ACL.
  auto self = [engine]() { return engine->SelfRequestor(); };

  // The desktop app uses this route only to decide whether the process is
  // alive. Keep it independent from storage, quota, and catalog locks so an
  // upload, recovery, or checksum scan cannot make a running node look offline.
  server->Get("/_ping", [](const HttpRequest&) -> HttpResponse {
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- document CRUD --------------------------------------------------------
  server->Put("/db/:collection/:key", [engine, network, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& key = req.params.at("key");

    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }

    CollectionMeta meta;
    if (engine->storage().catalog().GetCopy(collection, &meta) && meta.has_schema) {
      Status schema_st = ValidateAgainstSchema(body, meta.schema);
      if (!schema_st.ok()) return StatusError(schema_st);
    }

    // Durability options: local-only (1, default), or replicated:N with timeout.
    const uint32_t durability = static_cast<uint32_t>(std::max<int64_t>(1, QueryInt(req, "durability", 1)));
    const uint32_t timeout_ms = static_cast<uint32_t>(std::max<int64_t>(0, QueryInt(req, "timeout_ms", 5000)));

    // Generate message_id BEFORE the write so the broadcast and durability wait share it.
    const std::string message_id = MessageDedup::NewMessageId(engine->identity().node_id());
    network->SetNextBroadcastMessageId(message_id);

    Status st = engine->PutDocument(collection, key, body, self());
    if (!st.ok()) return StatusError(st);

    auto durability_report = engine->WaitForDurability(message_id, durability, timeout_ms);
    if (!durability_report.ok()) return StatusError(durability_report.status());

    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("key", JsonValue(key));
    obj.emplace_back("entry_id", JsonValue(static_cast<int64_t>(engine->LedgerTip().entry_id)));
    obj.emplace_back("message_id", JsonValue(message_id));
    const auto& report = durability_report.value();
    JsonValue::Object dur_obj;
    dur_obj.emplace_back("requested", JsonValue(static_cast<int64_t>(report.requested)));
    dur_obj.emplace_back("achieved", JsonValue(static_cast<int64_t>(report.achieved)));
    dur_obj.emplace_back("timed_out", JsonValue(report.timed_out));
    JsonValue::Array replicas_arr;
    for (const std::string& r : report.replicas) replicas_arr.emplace_back(JsonValue(r));
    dur_obj.emplace_back("replicas", JsonValue(std::move(replicas_arr)));
    obj.emplace_back("durability", JsonValue(std::move(dur_obj)));
    int status_code = report.timed_out && report.achieved < report.requested ? 202 : 200;
    return HttpResponse{status_code, "application/json", JsonValue(std::move(obj)).Dump()};
  });

  server->Get("/db/:collection/:key", [engine, self](const HttpRequest& req) -> HttpResponse {
    auto doc_or = engine->GetDocument(req.params.at("collection"), req.params.at("key"), self());
    if (!doc_or.ok()) return StatusError(doc_or.status());
    return JsonOk(doc_or.value());
  });

  server->Del("/db/:collection/:key", [engine, network, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& key = req.params.at("key");

    const uint32_t durability = static_cast<uint32_t>(std::max<int64_t>(1, QueryInt(req, "durability", 1)));
    const uint32_t timeout_ms = static_cast<uint32_t>(std::max<int64_t>(0, QueryInt(req, "timeout_ms", 5000)));

    // Generate message_id BEFORE the write so the broadcast and durability wait share it.
    const std::string message_id = MessageDedup::NewMessageId(engine->identity().node_id());
    network->SetNextBroadcastMessageId(message_id);

    Status st = engine->DeleteDocument(collection, key, self());
    if (!st.ok()) return StatusError(st);

    auto durability_report = engine->WaitForDurability(message_id, durability, timeout_ms);
    if (!durability_report.ok()) return StatusError(durability_report.status());

    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("key", JsonValue(key));
    obj.emplace_back("entry_id", JsonValue(static_cast<int64_t>(engine->LedgerTip().entry_id)));
    obj.emplace_back("message_id", JsonValue(message_id));
    const auto& report = durability_report.value();
    JsonValue::Object dur_obj;
    dur_obj.emplace_back("requested", JsonValue(static_cast<int64_t>(report.requested)));
    dur_obj.emplace_back("achieved", JsonValue(static_cast<int64_t>(report.achieved)));
    dur_obj.emplace_back("timed_out", JsonValue(report.timed_out));
    JsonValue::Array replicas_arr;
    for (const std::string& r : report.replicas) replicas_arr.emplace_back(JsonValue(r));
    dur_obj.emplace_back("replicas", JsonValue(std::move(replicas_arr)));
    obj.emplace_back("durability", JsonValue(std::move(dur_obj)));
    int status_code = report.timed_out && report.achieved < report.requested ? 202 : 200;
    return HttpResponse{status_code, "application/json", JsonValue(std::move(obj)).Dump()};
  });

  server->Get("/db/:collection", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string start_key = QueryString(req, "start");
    const std::string legacy_start = QueryString(req, "start_key");
    const size_t limit = static_cast<size_t>(std::max<int64_t>(0, QueryInt(req, "limit", 100)));

    const std::string& collection = req.params.at("collection");
    auto docs = engine->ListDocuments(collection, start_key.empty() ? legacy_start : start_key, limit,
                                       self());
    JsonValue::Array arr;
    for (auto& [key, doc] : docs) {
      JsonValue::Object entry;
      entry.emplace_back("key", JsonValue(key));
      entry.emplace_back("document", doc);
      arr.emplace_back(std::move(entry));
    }
    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("engine", JsonValue(engine->storage().router().EngineNameFor(collection)));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("documents", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- schema management -----------------------------------------------------
  server->Put("/_schema/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    JsonValue schema;
    try {
      schema = JsonValue::Parse(req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON schema: ") + e.what());
    }
    engine->storage().EnsureCollection(collection);
    Status st = engine->storage().catalog().SetSchema(collection, schema);
    if (!st.ok()) return StatusError(st);
    return Ok();
  });

  server->Get("/_schema/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    CollectionMeta meta;
    if (!engine->storage().catalog().GetCopy(req.params.at("collection"), &meta) || !meta.has_schema) {
      return JsonOk(JsonValue(nullptr));
    }
    return JsonOk(meta.schema);
  });

  // -- collection metadata: engine binding, ACL, retention, placement --------
  server->Get("/_collection/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    CollectionMeta meta;
    if (!engine->storage().catalog().GetCopy(collection, &meta)) {
      return JsonError(404, "no such collection: " + collection);
    }
    CollectionSummary summary = engine->Summarize(collection);

    JsonValue::Object obj;
    obj.emplace_back("name", JsonValue(collection));
    obj.emplace_back("engine", JsonValue(engine->storage().router().EngineNameFor(collection)));
    obj.emplace_back("document_count", JsonValue(static_cast<int64_t>(summary.document_count)));
    obj.emplace_back("checksum", JsonValue(summary.checksum_hex));
    obj.emplace_back("created_at_ms", JsonValue(static_cast<int64_t>(meta.created_at_ms)));
    obj.emplace_back("retention_days", JsonValue(static_cast<int64_t>(meta.retention_days)));
    obj.emplace_back("shard_key", JsonValue(meta.shard_key));
    obj.emplace_back("replication_factor", JsonValue(static_cast<int64_t>(meta.replication_factor)));
    obj.emplace_back("has_schema", JsonValue(meta.has_schema));
    obj.emplace_back("acl", AclToJson(engine->storage().catalog().EffectiveAcl(collection)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Put("/_collection/:collection/engine", [engine](const HttpRequest& req) -> HttpResponse {
    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    const JsonValue* name = body.Find("engine");
    if (name == nullptr || !name->is_string()) return JsonError(400, "body must be {\"engine\": \"...\"}");
    Status st = engine->storage().router().BindCollection(req.params.at("collection"), name->AsString());
    if (!st.ok()) return StatusError(st);
    return Ok();
  });

  server->Put("/_collection/:collection/acl", [engine](const HttpRequest& req) -> HttpResponse {
    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    const std::string& collection = req.params.at("collection");

    CollectionAcl acl;
    const JsonValue* owner = body.Find("owner_node");
    // Defaulting the owner to this node is the safe choice: a private
    // collection with no owner would be readable by nobody, including its
    // creator, which is never what the caller meant. Self-ownership is also
    // what lets peers verify the replicated ACL (origin == owner rule).
    acl.owner_node = (owner && owner->is_string() && !owner->AsString().empty())
                          ? owner->AsString()
                          : engine->identity().node_id();
    const JsonValue* is_private = body.Find("private");
    if (is_private && is_private->is_bool()) acl.is_private = is_private->AsBool();
    const JsonValue* readers = body.Find("readers");
    if (readers && readers->is_array()) {
      for (const JsonValue& r : readers->AsArray()) {
        if (r.is_string()) acl.readers.push_back(r.AsString());
      }
    }
    const JsonValue* parent = body.Find("parent");
    if (parent && parent->is_string()) acl.parent = parent->AsString();

    // Versioned + replicated: the ACL is appended as a signed kAcl record so
    // peers converge on it (H-1), not just stored locally.
    Status st = engine->SetCollectionAcl(collection, acl);
    if (!st.ok()) return StatusError(st);
    return Ok();
  });

  server->Put("/_collection/:collection/placement", [engine](const HttpRequest& req) -> HttpResponse {
    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    const std::string& collection = req.params.at("collection");
    engine->storage().EnsureCollection(collection);

    std::string shard_key;
    uint32_t rf = 0;
    const JsonValue* sk = body.Find("shard_key");
    if (sk && sk->is_string()) shard_key = sk->AsString();
    const JsonValue* factor = body.Find("replication_factor");
    if (factor && factor->is_number()) rf = static_cast<uint32_t>(factor->AsInt());

    Status st = engine->storage().catalog().SetPlacement(collection, shard_key, rf);
    if (!st.ok()) return StatusError(st);
    const JsonValue* retention = body.Find("retention_days");
    if (retention && retention->is_number()) {
      st = engine->storage().catalog().SetRetentionDays(collection,
                                                         static_cast<uint32_t>(retention->AsInt()));
      if (!st.ok()) return StatusError(st);
    }
    return Ok();
  });

  // Explicit retention run: POST /db/:collection/_retention/run applies the
  // collection's configured retention_days now and reports chunks dropped.
  // The engine also runs the same operation periodically; this endpoint is
  // useful when an operator wants the policy applied immediately.
  server->Post("/db/:collection/_retention/run", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    if (!engine->CanWrite(collection, self())) return StatusError(Status::AuthError("not allowed"));
    auto dropped_or = engine->RunRetention(collection);
    if (!dropped_or.ok()) return StatusError(dropped_or.status());
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("chunks_dropped", JsonValue(static_cast<int64_t>(dropped_or.value())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- engines available in this build ---------------------------------------
  server->Get("/_engines", [engine](const HttpRequest&) -> HttpResponse {
    // The app's engine picker calls this so it never offers a backend that
    // was not compiled in -- an option that fails on selection is worse than
    // an option that is not shown.
    static const char* kAllEngines[] = {"kv",        "columnar_lite", "ts_rollup", "vector_hnsw_lite",
                                        "graph_adj", "sqlite",        "duckdb",    "lmdb",
                                        "sqlite_vec"};
    const std::vector<std::string> loaded = engine->storage().router().AvailableEngines();

    JsonValue::Array arr;
    for (const char* name : kAllEngines) {
      const bool builtin = std::string(name) == "kv" || std::string(name) == "columnar_lite" ||
                           std::string(name) == "ts_rollup" ||
                           std::string(name) == "vector_hnsw_lite" || std::string(name) == "graph_adj";
      const bool compiled = builtin || VendoredBackendAvailable(name);
      const bool active = std::find(loaded.begin(), loaded.end(), name) != loaded.end();

      JsonValue::Object o;
      o.emplace_back("name", JsonValue(name));
      o.emplace_back("built_in", JsonValue(builtin));
      o.emplace_back("compiled_in", JsonValue(compiled));
      o.emplace_back("active_on_this_node", JsonValue(active));
      if (!compiled) o.emplace_back("enable_with", JsonValue(VendoredBackendOption(name)));
      arr.emplace_back(std::move(o));
    }
    return JsonOk(JsonValue(std::move(arr)));
  });

  // -- collections / cluster introspection ------------------------------------
  server->Get("/_collections", [engine, self](const HttpRequest&) -> HttpResponse {
    JsonValue::Array arr;
    for (const std::string& name : engine->ReadableCollections(self())) arr.emplace_back(JsonValue(name));
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Get("/_peers", [network](const HttpRequest&) -> HttpResponse {
    const lsn_t network_max = network->peers().NetworkMaxLedgerEntryId();
    const int64_t now_ms = NowMs();
    const int64_t liveness_threshold_ms = network->config().liveness_threshold_ms;
    JsonValue::Array arr;
    for (const PeerInfo& p : network->peers().Ranked())
      arr.emplace_back(PeerToJson(p, network_max, now_ms, liveness_threshold_ms));
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Get("/_peer_sync", [network](const HttpRequest&) -> HttpResponse {
    JsonValue::Array snapshots;
    for (const NetworkManager::PeerSyncSnapshot& snapshot : network->PeerSyncSnapshots()) {
      JsonValue::Object peer;
      peer.emplace_back("node_id", JsonValue(snapshot.node_id));
      peer.emplace_back("observed_at_ms", JsonValue(snapshot.observed_at_ms));
      JsonValue::Array collections;
      for (const auto& [name, checksum] : snapshot.collections) {
        JsonValue::Object collection;
        collection.emplace_back("name", JsonValue(name));
        collection.emplace_back("checksum", JsonValue(checksum));
        collections.emplace_back(std::move(collection));
      }
      peer.emplace_back("collections", JsonValue(std::move(collections)));
      snapshots.emplace_back(std::move(peer));
    }
    return JsonOk(JsonValue(std::move(snapshots)));
  });

  server->Get("/_status", [engine, network](const HttpRequest&) -> HttpResponse {
    const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    const StorageEngine::QuotaStatus quota = engine->Quota();
    const NetworkManager::BroadcastStats bcast = network->broadcast_stats();

    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("node_name", JsonValue(network->config().node_name));
    obj.emplace_back("public_key_hex", JsonValue(HexEncode(engine->identity().public_key())));
    obj.emplace_back("uptime_seconds", JsonValue(static_cast<int64_t>(uptime_s)));
    obj.emplace_back("supervisor", JsonValue(engine->is_supervisor()));
    // At-rest posture, honestly reported: whether this node seals its files.
    // A stolen disk of a node reporting false here is readable; true means
    // every data file needs the unlock key.
    obj.emplace_back("encrypt_at_rest", JsonValue(network->config().encrypt_at_rest));
    obj.emplace_back("at_rest_sealed", JsonValue(engine->at_rest_sealed()));
    // Membership posture (the secret itself is never exposed): closed means
    // HELLOs and beacons without a valid tag are refused/ignored.
    obj.emplace_back("membership_required", JsonValue(network->membership_required()));
    obj.emplace_back("collections", JsonValue(static_cast<int64_t>(engine->ListCollections().size())));
    obj.emplace_back("known_peers", JsonValue(static_cast<int64_t>(network->peers().Size())));
    obj.emplace_back("replication_factor",
                     JsonValue(static_cast<int64_t>(engine->replication_factor())));
    obj.emplace_back("default_engine", JsonValue(network->config().default_engine));
    obj.emplace_back("api_port", JsonValue(static_cast<int64_t>(network->config().api_port)));
    obj.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(network->config().p2p_port)));

    const NodeEngine::RetentionSweepReport retention = engine->RetentionStatus();
    JsonValue::Object retention_obj;
    retention_obj.emplace_back("last_run_ms", JsonValue(static_cast<int64_t>(retention.last_run_ms)));
    retention_obj.emplace_back("collections_considered",
                               JsonValue(static_cast<int64_t>(retention.collections_considered)));
    retention_obj.emplace_back("chunks_dropped",
                               JsonValue(static_cast<int64_t>(retention.chunks_dropped)));
    retention_obj.emplace_back("last_error", JsonValue(retention.last_error));
    obj.emplace_back("retention", JsonValue(std::move(retention_obj)));

    JsonValue::Object quota_obj;
    quota_obj.emplace_back("limit_bytes", JsonValue(static_cast<int64_t>(quota.limit_bytes)));
    quota_obj.emplace_back("used_bytes", JsonValue(static_cast<int64_t>(quota.used_bytes)));
    quota_obj.emplace_back("ledger_bytes", JsonValue(static_cast<int64_t>(quota.ledger_bytes)));
    quota_obj.emplace_back("used_fraction", JsonValue(quota.used_fraction));
    quota_obj.emplace_back("over_limit", JsonValue(quota.over_limit));
    obj.emplace_back("quota", JsonValue(std::move(quota_obj)));

    JsonValue::Object bcast_obj;
    bcast_obj.emplace_back("sent", JsonValue(static_cast<int64_t>(bcast.sent)));
    bcast_obj.emplace_back("dropped", JsonValue(static_cast<int64_t>(bcast.dropped)));
    bcast_obj.emplace_back("duplicates_suppressed",
                           JsonValue(static_cast<int64_t>(bcast.duplicates_suppressed)));
    bcast_obj.emplace_back("rate_limited", JsonValue(static_cast<int64_t>(bcast.rate_limited)));
    bcast_obj.emplace_back("queued", JsonValue(static_cast<int64_t>(bcast.queued)));
    obj.emplace_back("broadcast", JsonValue(std::move(bcast_obj)));

    const NetworkManager::ProbeStats probes = network->probe_stats();
    JsonValue::Object probes_obj;
    probes_obj.emplace_back("completed", JsonValue(static_cast<int64_t>(probes.completed)));
    probes_obj.emplace_back("dropped", JsonValue(static_cast<int64_t>(probes.dropped)));
    probes_obj.emplace_back("queued", JsonValue(static_cast<int64_t>(probes.queued)));
    obj.emplace_back("probes", JsonValue(std::move(probes_obj)));

    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_quota", [engine](const HttpRequest&) -> HttpResponse {
    const StorageEngine::QuotaStatus quota = engine->Quota();
    JsonValue::Object obj;
    obj.emplace_back("limit_bytes", JsonValue(static_cast<int64_t>(quota.limit_bytes)));
    obj.emplace_back("used_bytes", JsonValue(static_cast<int64_t>(quota.used_bytes)));
    obj.emplace_back("ledger_bytes", JsonValue(static_cast<int64_t>(quota.ledger_bytes)));
    obj.emplace_back("used_fraction", JsonValue(quota.used_fraction));
    obj.emplace_back("over_limit", JsonValue(quota.over_limit));

    JsonValue::Array engines;
    for (const std::string& name : engine->storage().router().AvailableEngines()) {
      EngineBackend* backend = engine->storage().router().Backend(name);
      if (backend == nullptr) continue;
      JsonValue::Object e;
      e.emplace_back("engine", JsonValue(name));
      e.emplace_back("used_bytes", JsonValue(static_cast<int64_t>(backend->QuotaUse())));
      e.emplace_back("limit_bytes", JsonValue(static_cast<int64_t>(backend->QuotaLimit())));
      engines.emplace_back(std::move(e));
    }
    obj.emplace_back("engines", JsonValue(std::move(engines)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- hash-chained audit ledger ----------------------------------------------
  server->Get("/_ledger/tip", [engine](const HttpRequest&) -> HttpResponse {
    const auto tip = engine->LedgerTip();
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("entry_id", JsonValue(static_cast<int64_t>(tip.entry_id)));
    obj.emplace_back("entry_hash", JsonValue(HexEncode(tip.entry_hash)));
    obj.emplace_back("signature", JsonValue(HexEncode(engine->SignLedgerTip())));
    obj.emplace_back("public_key_hex", JsonValue(HexEncode(engine->identity().public_key())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_ledger/entries", [engine](const HttpRequest& req) -> HttpResponse {
    lsn_t from = static_cast<lsn_t>(QueryInt(req, "from", 0));
    lsn_t to = static_cast<lsn_t>(QueryInt(req, "to", -1));
    const lsn_t tip_id = engine->LedgerTip().entry_id;
    if (to < 0 || to > tip_id) to = tip_id;
    if (from < 0) from = 0;
    // A debug/audit endpoint, not a bulk-export path: a peer wanting a full
    // replay pages through it.
    constexpr int64_t kMaxRange = 5000;
    if (to - from + 1 > kMaxRange) to = from + kMaxRange - 1;

    auto entries_or = engine->LedgerEntries(from, to);
    if (!entries_or.ok()) return StatusError(entries_or.status());

    JsonValue::Array arr;
    for (const WalRecord& rec : entries_or.value()) arr.emplace_back(LedgerEntryToJson(rec));
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("from", JsonValue(static_cast<int64_t>(from)));
    obj.emplace_back("to", JsonValue(static_cast<int64_t>(to)));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("entries", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Post("/_ledger/verify", [engine](const HttpRequest&) -> HttpResponse {
    const WriteAheadLog::VerifyResult result = engine->VerifyLedger();
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("verified", JsonValue(result.ok));
    obj.emplace_back("entries_checked", JsonValue(static_cast<int64_t>(result.entries_checked)));
    obj.emplace_back("signed_entries", JsonValue(static_cast<int64_t>(result.signed_entries)));
    obj.emplace_back("unsigned_entries", JsonValue(static_cast<int64_t>(result.unsigned_entries)));
    if (!result.ok) {
      obj.emplace_back("failed_at_entry_id", JsonValue(static_cast<int64_t>(result.failed_at_entry_id)));
      obj.emplace_back("reason", JsonValue(result.reason));
    }
    return JsonOk(JsonValue(std::move(obj)));
  });

  // Full integrity check: the ledger chain *and* every storage backend's own
  // structures. The ledger proves history was not rewritten; the backends
  // prove the materialised state is actually readable. Reporting only the
  // first would be misleading.
  server->Post("/_verify", [engine](const HttpRequest&) -> HttpResponse {
    const StorageEngine::VerifyReport report = engine->VerifyEverything();
    JsonValue::Object ledger;
    ledger.emplace_back("verified", JsonValue(report.ledger.ok));
    ledger.emplace_back("entries_checked", JsonValue(static_cast<int64_t>(report.ledger.entries_checked)));
    ledger.emplace_back("signed_entries", JsonValue(static_cast<int64_t>(report.ledger.signed_entries)));
    ledger.emplace_back("unsigned_entries",
                        JsonValue(static_cast<int64_t>(report.ledger.unsigned_entries)));
    if (!report.ledger.ok) ledger.emplace_back("reason", JsonValue(report.ledger.reason));

    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("ok", JsonValue(report.ledger.ok && report.backends_ok));
    obj.emplace_back("ledger", JsonValue(std::move(ledger)));
    obj.emplace_back("backends_ok", JsonValue(report.backends_ok));
    if (!report.backends_ok) obj.emplace_back("backend_failure", JsonValue(report.backend_failure));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- live change feed -------------------------------------------------------
  // Long poll over the ledger tip. The app's tree, mesh and explorer views all
  // subscribe to this rather than polling /_brain on a timer.
  server->Get("/_changes", [engine](const HttpRequest& req) -> HttpResponse {
    const lsn_t since = static_cast<lsn_t>(QueryInt(req, "since", -1));
    const uint32_t timeout_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, std::min<int64_t>(ChangeFeed::kMaxTimeoutMs,
                                                QueryInt(req, "timeout_ms", 25000))));
    const size_t limit = static_cast<size_t>(std::max<int64_t>(0, QueryInt(req, "limit", 256)));

    auto batch_or = engine->changes().Wait(since, timeout_ms, limit);
    if (!batch_or.ok()) return StatusError(batch_or.status());
    const ChangeBatch& batch = batch_or.value();

    JsonValue::Array arr;
    for (const WalRecord& rec : batch.entries) arr.emplace_back(LedgerEntryToJson(rec));

    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("since", JsonValue(static_cast<int64_t>(batch.from)));
    obj.emplace_back("tip", JsonValue(static_cast<int64_t>(batch.tip)));
    // `truncated` tells a client its cursor predates a prune, so it re-syncs
    // from scratch instead of believing it has seen everything.
    obj.emplace_back("truncated", JsonValue(batch.truncated));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("changes", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- transit (offline-owner flow) -------------------------------------------
  server->Get("/_transit", [engine](const HttpRequest&) -> HttpResponse {
    JsonValue::Array owners;
    uint64_t documents = 0;
    for (const std::string& owner : engine->transit().Owners()) {
      const std::vector<TransitEnvelope> pending = engine->PendingTransitFor(owner);
      documents += pending.size();
      JsonValue::Object o;
      o.emplace_back("owner_node", JsonValue(owner));
      o.emplace_back("documents", JsonValue(static_cast<int64_t>(pending.size())));
      int64_t soonest_expiry = 0;
      for (const TransitEnvelope& envelope : pending) {
        if (envelope.expires_ms == 0) continue;
        if (soonest_expiry == 0 || envelope.expires_ms < soonest_expiry) {
          soonest_expiry = envelope.expires_ms;
        }
      }
      o.emplace_back("soonest_expiry_ms", JsonValue(soonest_expiry));
      owners.emplace_back(std::move(o));
    }

    JsonValue::Object obj;
    obj.emplace_back("holding_for", JsonValue(std::move(owners)));
    obj.emplace_back("documents_held", JsonValue(static_cast<int64_t>(documents)));
    obj.emplace_back("bytes_held", JsonValue(static_cast<int64_t>(engine->transit().BytesHeld())));
    obj.emplace_back("ttl_seconds", JsonValue(static_cast<int64_t>(engine->transit().ttl_seconds())));
    // True when the transit log met present-but-invalid bytes at open (bad
    // CRC or implausible length) rather than a clean EOF or torn tail. The
    // corrupt tail was dropped and the log rewritten; this flag is the
    // operator-visible record that corruption happened.
    obj.emplace_back("load_corrupt", JsonValue(engine->TransitLoadCorrupt()));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Post("/_transit/claim", [network](const HttpRequest&) -> HttpResponse {
    const NetworkManager::ClaimReport report = network->ClaimPendingTransit();
    JsonValue::Object obj;
    obj.emplace_back("peers_asked", JsonValue(static_cast<int64_t>(report.peers_asked)));
    obj.emplace_back("documents_claimed", JsonValue(static_cast<int64_t>(report.documents_claimed)));
    obj.emplace_back("failures", JsonValue(static_cast<int64_t>(report.failures)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Post("/_transit/expire", [engine](const HttpRequest&) -> HttpResponse {
    auto dropped_or = engine->transit().ExpireAsOf(NowMs());
    if (!dropped_or.ok()) return StatusError(dropped_or.status());
    JsonValue::Object obj;
    obj.emplace_back("expired", JsonValue(static_cast<int64_t>(dropped_or.value())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- outbox (stage-anywhere, sync-on-reconnect flow) --------------------------
  server->Get("/_outbox", [engine](const HttpRequest&) -> HttpResponse {
    JsonValue::Object obj;
    obj.emplace_back("entries_staged", JsonValue(static_cast<int64_t>(engine->OutboxSize())));
    obj.emplace_back("bytes_staged", JsonValue(static_cast<int64_t>(engine->OutboxBytesHeld())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Post("/_outbox/flush", [engine](const HttpRequest&) -> HttpResponse {
    const size_t replayed = engine->FlushOutbox();
    JsonValue::Object obj;
    obj.emplace_back("replayed", JsonValue(static_cast<int64_t>(replayed)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- checkpoint (supervisor only) -------------------------------------------
  server->Post("/_checkpoint", [engine, network](const HttpRequest&) -> HttpResponse {
    // Checkpointing prunes the ledger, so it is gated to supervisors: a data
    // node running it on its own would be exactly the "one node decides to
    // delete history" failure the quorum gate exists to prevent.
    if (!engine->is_supervisor()) {
      return JsonError(403, "checkpointing runs on a supervisor node; this node is not one");
    }
    const auto tip = engine->LedgerTip();
    ReplicaTip local;
    local.node_id = engine->identity().node_id();
    local.entry_id = tip.entry_id;
    local.entry_hash = tip.entry_hash;
    local.signature = engine->SignLedgerTip();
    local.self_verified = engine->VerifyLedger().ok;
    local.signature_valid = true;  // we produced it

    auto outcome_or = RunCheckpoint(engine->storage().wal(), &engine->transit(), local,
                                     network->CollectReplicaTips(), engine->replication_factor(),
                                     engine->clock().Now());
    if (!outcome_or.ok()) return StatusError(outcome_or.status());
    const CheckpointOutcome& outcome = outcome_or.value();

    JsonValue::Object decision;
    decision.emplace_back("proceeded", JsonValue(outcome.decision.proceed));
    decision.emplace_back("agreeing", JsonValue(static_cast<int64_t>(outcome.decision.agreeing)));
    decision.emplace_back("required", JsonValue(static_cast<int64_t>(outcome.decision.required)));
    decision.emplace_back("conflicting", JsonValue(static_cast<int64_t>(outcome.decision.conflicting)));
    decision.emplace_back("unverified", JsonValue(static_cast<int64_t>(outcome.decision.unverified)));
    decision.emplace_back("checkpoint_lsn",
                          JsonValue(static_cast<int64_t>(outcome.decision.checkpoint_lsn)));
    if (!outcome.decision.reason.empty()) {
      decision.emplace_back("reason", JsonValue(outcome.decision.reason));
    }

    JsonValue::Object obj;
    obj.emplace_back("decision", JsonValue(std::move(decision)));
    obj.emplace_back("checkpoint_entry_id",
                     JsonValue(static_cast<int64_t>(outcome.checkpoint_entry_id)));
    obj.emplace_back("entries_pruned", JsonValue(static_cast<int64_t>(outcome.entries_pruned)));
    obj.emplace_back("envelopes_released",
                     JsonValue(static_cast<int64_t>(outcome.envelopes_released)));
    obj.emplace_back("previous_tip_hash", JsonValue(HexEncode(outcome.previous_tip_hash)));
    obj.emplace_back("new_tip_hash", JsonValue(HexEncode(outcome.new_tip_hash)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- placement --------------------------------------------------------------
  server->Get("/_placement/:collection/:key", [engine, network](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& key = req.params.at("key");

    std::string shard_value;
    CollectionMeta meta;
    if (engine->storage().catalog().GetCopy(collection, &meta) && !meta.shard_key.empty()) {
      auto doc = engine->GetDocument(collection, key, engine->SelfRequestor());
      if (doc.ok() && doc.value().is_object()) {
        const JsonValue* v = doc.value().Find(meta.shard_key);
        if (v != nullptr && v->is_string()) shard_value = v->AsString();
      }
    }

    const PlacementPlan plan = network->placement().Place(collection, key, shard_value);
    JsonValue::Array replicas;
    for (const std::string& r : plan.replicas) replicas.emplace_back(JsonValue(r));
    JsonValue::Array skipped;
    for (const std::string& s : plan.skipped) skipped.emplace_back(JsonValue(s));
    JsonValue::Array displaced;
    for (const std::string& d : plan.displaced_owners) displaced.emplace_back(JsonValue(d));

    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("key", JsonValue(key));
    obj.emplace_back("hash_input", JsonValue(plan.key));
    obj.emplace_back("shard_key", JsonValue(meta.shard_key));
    obj.emplace_back("replicas", JsonValue(std::move(replicas)));
    obj.emplace_back("primary", JsonValue(plan.primary()));
    obj.emplace_back("requested_rf", JsonValue(static_cast<int64_t>(plan.requested_rf)));
    obj.emplace_back("under_replicated", JsonValue(plan.under_replicated));
    obj.emplace_back("ring_nodes",
                     JsonValue(static_cast<int64_t>(network->placement().ring().NodeCount())));
    obj.emplace_back("skipped", JsonValue(std::move(skipped)));
    obj.emplace_back("displaced_owners", JsonValue(std::move(displaced)));
    // Honesty note, stated in the response rather than buried in docs:
    // `replicas` is the ring's RF subset for this key, but writes are
    // broadcast to every connected peer and gossip converges the rest, so in
    // practice every reachable data node holds every key. Compare
    // per-collection checksums in /_brain across nodes for the ground truth
    // of who actually holds what.
    obj.emplace_back("replication_note",
                     JsonValue("ring RF subset; actual holders converge to all reachable data "
                               "nodes via broadcast + gossip -- see /_brain checksums"));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- engine-specific query surfaces -----------------------------------------
  server->Post("/_search/vector/:collection", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    if (!engine->CanRead(collection, self())) return JsonError(403, "not permitted");

    auto* backend = dynamic_cast<VectorHnswLiteBackend*>(
        engine->storage().router().Backend("vector_hnsw_lite"));
    if (backend == nullptr) return JsonError(400, "the vector_hnsw_lite engine is not active on this node");

    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }
    const JsonValue* vector = body.Find("vector");
    if (vector == nullptr || !vector->is_array()) {
      return JsonError(400, "body must be {\"vector\": [...], \"k\": n}");
    }
    std::vector<float> query;
    query.reserve(vector->AsArray().size());
    for (const JsonValue& v : vector->AsArray()) {
      if (!v.is_number()) return JsonError(400, "vector components must be numbers");
      query.push_back(static_cast<float>(v.AsDouble()));
    }
    const JsonValue* k = body.Find("k");
    const size_t top_k = (k && k->is_number()) ? static_cast<size_t>(k->AsInt()) : 10;

    auto hits_or = backend->Search(collection, query, top_k);
    if (!hits_or.ok()) return StatusError(hits_or.status());

    JsonValue::Array arr;
    for (const VectorHit& hit : hits_or.value()) {
      JsonValue::Object o;
      o.emplace_back("key", JsonValue(hit.key));
      o.emplace_back("score", JsonValue(static_cast<double>(hit.score)));
      arr.emplace_back(std::move(o));
    }
    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("hits", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_ts/:collection/rollups", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    if (!engine->CanRead(collection, self())) return JsonError(403, "not permitted");

    auto* backend = dynamic_cast<TsRollupBackend*>(engine->storage().router().Backend("ts_rollup"));
    if (backend == nullptr) return JsonError(400, "the ts_rollup engine is not active on this node");

    const std::string series = QueryString(req, "series");
    const int64_t from_ms = QueryInt(req, "from_ms", 0);
    const int64_t to_ms = QueryInt(req, "to_ms", NowMs());
    const int64_t bucket_ms = QueryInt(req, "bucket_ms", kTsRollupBucketMs);

    JsonValue::Object out;
    for (const auto& [name, buckets] : backend->Rollups(collection, series, from_ms, to_ms, bucket_ms)) {
      JsonValue::Array arr;
      for (const TsRollupBucket& b : buckets) {
        JsonValue::Object o;
        o.emplace_back("bucket_start_ms", JsonValue(b.bucket_start_ms));
        o.emplace_back("count", JsonValue(static_cast<int64_t>(b.count)));
        o.emplace_back("min", JsonValue(b.min));
        o.emplace_back("max", JsonValue(b.max));
        o.emplace_back("sum", JsonValue(b.sum));
        o.emplace_back("first", JsonValue(b.first));
        o.emplace_back("last", JsonValue(b.last));
        arr.emplace_back(std::move(o));
      }
      out.emplace_back(name, JsonValue(std::move(arr)));
    }
    return JsonOk(JsonValue(std::move(out)));
  });

  server->Get("/_graph/:collection/:key", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    if (!engine->CanRead(collection, self())) return JsonError(403, "not permitted");

    auto* backend = dynamic_cast<GraphAdjBackend*>(engine->storage().router().Backend("graph_adj"));
    if (backend == nullptr) return JsonError(400, "the graph_adj engine is not active on this node");

    const std::string& key = req.params.at("key");
    const size_t depth = static_cast<size_t>(std::max<int64_t>(0, QueryInt(req, "depth", 1)));

    JsonValue::Array children;
    for (const GraphEdge& e : backend->OutEdges(collection, key)) {
      JsonValue::Object o;
      o.emplace_back("to", JsonValue(e.to));
      o.emplace_back("label", JsonValue(e.label));
      children.emplace_back(std::move(o));
    }
    JsonValue::Array parents;
    for (const GraphEdge& e : backend->InEdges(collection, key)) {
      parents.emplace_back(JsonValue(e.to));
    }
    JsonValue::Array descendants;
    for (const std::string& d : backend->Descendants(collection, key, depth)) {
      descendants.emplace_back(JsonValue(d));
    }

    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("key", JsonValue(key));
    obj.emplace_back("out_edges", JsonValue(std::move(children)));
    obj.emplace_back("in_edges", JsonValue(std::move(parents)));
    obj.emplace_back("descendants", JsonValue(std::move(descendants)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- brain file: a compact, human/agent-readable snapshot of this node's
  // entire state, so a peer (or an agent orchestrating the mesh) can get full
  // awareness without running expensive queries against every node.
  server->Get("/_brain", [engine, network, self](const HttpRequest&) -> HttpResponse {
    const auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
    const auto tip = engine->LedgerTip();
    const StorageEngine::QuotaStatus quota = engine->Quota();

    JsonValue::Array collections;
    for (const std::string& name : engine->ReadableCollections(self())) {
      const CollectionSummary summary = engine->Summarize(name);
      JsonValue::Object c;
      c.emplace_back("name", JsonValue(summary.name));
      c.emplace_back("engine", JsonValue(summary.engine));
      c.emplace_back("private", JsonValue(summary.is_private));
      c.emplace_back("document_count", JsonValue(static_cast<int64_t>(summary.document_count)));
      c.emplace_back("checksum", JsonValue(summary.checksum_hex));
      collections.emplace_back(std::move(c));
    }

    const lsn_t network_max = network->peers().NetworkMaxLedgerEntryId();
    const int64_t peers_now_ms = NowMs();
    const int64_t peers_liveness_ms = network->config().liveness_threshold_ms;
    JsonValue::Array peers;
    for (const PeerInfo& p : network->peers().Ranked())
      peers.emplace_back(PeerToJson(p, network_max, peers_now_ms, peers_liveness_ms));

    JsonValue::Object ledger_tip;
    ledger_tip.emplace_back("entry_id", JsonValue(static_cast<int64_t>(tip.entry_id)));
    ledger_tip.emplace_back("entry_hash", JsonValue(HexEncode(tip.entry_hash)));
    ledger_tip.emplace_back("signature", JsonValue(HexEncode(engine->SignLedgerTip())));

    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("node_name", JsonValue(network->config().node_name));
    obj.emplace_back("supervisor", JsonValue(engine->is_supervisor()));
    obj.emplace_back("generated_at_us",
                     JsonValue(static_cast<int64_t>(
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count())));
    obj.emplace_back("uptime_seconds", JsonValue(static_cast<int64_t>(uptime_s)));
    obj.emplace_back("ledger_tip", JsonValue(std::move(ledger_tip)));
    obj.emplace_back("free_quota_mb",
                     JsonValue(static_cast<int64_t>(
                         quota.limit_bytes > quota.used_bytes
                             ? (quota.limit_bytes - quota.used_bytes) / (1024 * 1024)
                             : 0)));
    obj.emplace_back("collections", JsonValue(std::move(collections)));
    obj.emplace_back("known_peers", JsonValue(std::move(peers)));
    obj.emplace_back("transit_documents_held",
                     JsonValue(static_cast<int64_t>(engine->transit().Size())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- effective configuration, for the app's node inspector -----------------
  server->Get("/_config", [network](const HttpRequest&) -> HttpResponse {
    // Returns the config as the node actually parsed it, not as the file was
    // written -- so a field the user mistyped shows its default here, which
    // is exactly the question "why isn't my setting taking effect?" needs.
    HttpResponse resp;
    resp.status = 200;
    resp.content_type = "application/json";
    resp.body = network->config().ToJson();
    return resp;
  });

  // -- secondary index lookup (equality on a single field) ------------------
  // MUST be registered BEFORE /_lookup/:collection or the more general
  // pattern will consume requests like /_lookup/users/email?value=...
  server->Get("/_lookup/:collection/:index", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& index_field = req.params.at("index");
    const std::string value = QueryString(req, "value");

    if (value.empty()) {
      return JsonError(400, "missing required query parameter: value");
    }

    if (!engine->CanRead(collection, self())) {
      return JsonError(404, "collection not found or not readable: " + collection);
    }

    const int64_t limit_raw = QueryInt(req, "limit", 100);
    const size_t limit = static_cast<size_t>(std::clamp(limit_raw, int64_t(1), int64_t(1000)));

    auto keys_or = engine->storage().router().QueryIndex(collection, index_field, value, limit);
    if (!keys_or.ok()) return StatusError(keys_or.status());

    const std::vector<std::string>& keys = keys_or.value();
    JsonValue::Array results;
    for (const std::string& key : keys) {
      auto doc_or = engine->GetDocument(collection, key, self());
      if (!doc_or.ok()) continue;

      JsonValue::Object entry;
      entry.emplace_back("key", JsonValue(key));
      entry.emplace_back("value", doc_or.value());
      results.emplace_back(std::move(entry));
    }

    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("index_field", JsonValue(index_field));
    obj.emplace_back("query_value", JsonValue(value));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(results.size())));
    obj.emplace_back("results", JsonValue(std::move(results)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- universal lookup: search collection names and document keys -----------
  server->Get("/_lookup/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string query = QueryString(req, "query");
    const int64_t limit_raw = QueryInt(req, "limit", 200);
    const size_t limit = static_cast<size_t>(std::clamp(limit_raw, int64_t(1), int64_t(1000)));
    const std::string start = QueryString(req, "start");

    const Requestor who = engine->SelfRequestor();

    // Resolve which collections to search.
    // If collection is "*" or empty, search across all readable collections,
    // filtering by collection name match if query is non-empty.
    std::vector<std::string> collections;
    if (collection.empty() || collection == "*") {
      auto all = engine->ReadableCollections(who);
      if (query.empty()) {
        collections = std::move(all);
      } else {
        // First pass: collections whose names match the query
        for (const std::string& name : all) {
          if (name == query || name.rfind(query, 0) == 0 || name.find(query) != std::string::npos) {
            collections.push_back(name);
          }
        }
        // If no collections matched by name, search all collections for key matches
        if (collections.empty()) {
          collections = all;
        }
      }
    } else {
      if (!engine->CanRead(collection, who)) {
        return JsonError(404, "collection not found or not readable: " + collection);
      }
      collections.push_back(collection);
    }

    Catalog& catalog = engine->storage().catalog();

    struct Hit {
      std::string collection;
      std::string key;
      std::string preview;
      std::string engine;
      std::string last_modified;
      int tier;        // 0 = exact, 1 = prefix, 2 = substring
      uint64_t physical_ms;
    };

    std::vector<Hit> hits;
    // Per-collection scan cap keeps memory bounded on huge collections.
    const size_t per_collection_cap = limit * 4 + 64;

    for (const std::string& coll : collections) {
      const CollectionMeta* meta = catalog.Get(coll);
      const std::string engine_name =
          (meta && !meta->engine.empty()) ? meta->engine : engine->storage().router().EngineNameFor(coll);

      const bool coll_name_matches = query.empty() || coll == query || coll.rfind(query, 0) == 0 || coll.find(query) != std::string::npos;

      const size_t fetch_limit = per_collection_cap;
      for (auto& [key, bytes] : engine->storage().Scan(coll, start, fetch_limit)) {
        CrdtValue doc = CrdtValue::Decode(bytes);
        if (doc.IsEmpty()) continue;

        int tier = -1;
        if (query.empty()) {
          tier = 2;
        } else if (key == query) {
          tier = 0;
        } else if (key.rfind(query, 0) == 0) {
          tier = 1;
        } else if (key.find(query) != std::string::npos) {
          tier = 2;
        } else if (coll_name_matches) {
          // Collection name matched query; include all keys from this collection
          tier = 2;
        } else {
          continue;
        }

        std::string json = doc.ToJson().Dump();
        std::string preview = json.size() > 200 ? json.substr(0, 200) : json;

        hits.push_back(Hit{coll, key, preview, engine_name,
                           doc.ts().ToString(), tier, doc.ts().physical_ms});

        if (hits.size() >= per_collection_cap) break;
      }
    }

    // Rank: exact > prefix > substring, then recency (physical_ms descending).
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
      if (a.tier != b.tier) return a.tier < b.tier;
      return a.physical_ms > b.physical_ms;
    });

    if (hits.size() > limit) hits.resize(limit);

    JsonValue::Array arr;
    for (const auto& h : hits) {
      JsonValue::Object obj;
      obj.emplace_back("collection", JsonValue(h.collection));
      obj.emplace_back("key", JsonValue(h.key));
      obj.emplace_back("preview", JsonValue(h.preview));
      obj.emplace_back("engine", JsonValue(h.engine));
      obj.emplace_back("last_modified", JsonValue(h.last_modified));
      arr.emplace_back(JsonValue(std::move(obj)));
    }

    const size_t result_count = arr.size();
    JsonValue::Object obj;
    obj.emplace_back("results", JsonValue(std::move(arr)));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(result_count)));
    obj.emplace_back("query", JsonValue(query));
    obj.emplace_back("collection", JsonValue(collection));
    return JsonOk(JsonValue(std::move(obj)));
  });
  // Register additional file browsing routes
  RegisterFileRoutes(server, engine, network, self);
}

// -- universal file lookup / access ------------------------------------------
void RegisterFileRoutes(HttpServer* server, NodeEngine* engine, NetworkManager* network,
                         std::function<Requestor()> self) {
  // GET /_files - list files across collections
  server->Get("/_files", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string collection_filter = QueryString(req, "collection");
    const std::string mime_filter = QueryString(req, "mime");
    const int64_t min_size = QueryInt(req, "min_size", 0);
    const int64_t max_size = QueryInt(req, "max_size", -1);
    const int64_t before_ms = QueryInt(req, "before_ms", 0);
    const int64_t after_ms = QueryInt(req, "after_ms", 0);
    const int64_t limit = std::max<int64_t>(1, QueryInt(req, "limit", 100));
    const int64_t offset = std::max<int64_t>(0, QueryInt(req, "offset", 0));
    const std::string sort = QueryString(req, "sort", "created_ms");

    std::vector<std::string> collections;
    if (collection_filter.empty()) {
      collections = engine->ListCollections();
    } else {
      if (engine->CanRead(collection_filter, self())) {
        collections.push_back(collection_filter);
      }
    }

    struct FileInfo {
      std::string collection;
      std::string key;
      std::string filename;
      std::string mime;
      uint64_t size_bytes;
      std::string sha256;
      int64_t created_ms;
      bool chunked;
      int64_t chunk_total;
    };

    std::vector<FileInfo> files;
    for (const std::string& coll : collections) {
      if (!engine->CanRead(coll, self())) continue;

      // File manifests can be separated by hundreds of chunk documents in
      // key order. A bounded prefix silently hides later files, so scan the
      // collection before filtering and paginating the much smaller file set.
      auto docs = engine->ListDocuments(coll, "", 0, self());

      for (auto& [key, doc] : docs) {
        if (!doc.is_object()) continue;

        // Check if it's a file asset (has filename field)
        const JsonValue* _fn = doc.Find("filename");
        const std::string filename = (_fn && _fn->is_string()) ? _fn->AsString() : "";
        if (filename.empty()) continue;

        const JsonValue* _mi = doc.Find("mime");
        const std::string mime = (_mi && _mi->is_string()) ? _mi->AsString() : "application/octet-stream";
        const JsonValue* _sz = doc.Find("size_bytes");
        const int64_t size_bytes = (_sz && _sz->is_number()) ? _sz->AsInt() : 0;
        const JsonValue* _sh = doc.Find("sha256");
        const std::string sha256 = (_sh && _sh->is_string()) ? _sh->AsString() : "";
        const JsonValue* _cm = doc.Find("created_ms");
        const int64_t created_ms = (_cm && _cm->is_number()) ? _cm->AsInt() : 0;
        const JsonValue* _ch = doc.Find("chunked");
        const bool chunked = (_ch && _ch->is_bool()) ? _ch->AsBool() : false;
        const JsonValue* _ct = doc.Find("chunk_total");
        const int64_t chunk_total = (_ct && _ct->is_number()) ? _ct->AsInt() : 0;

        // Apply filters
        if (!mime_filter.empty() && mime.find(mime_filter) != 0) continue;
        if (min_size > 0 && static_cast<int64_t>(size_bytes) < min_size) continue;
        if (max_size > 0 && static_cast<int64_t>(size_bytes) > max_size) continue;
        if (before_ms > 0 && created_ms >= before_ms) continue;
        if (after_ms > 0 && created_ms <= after_ms) continue;

        files.push_back(FileInfo{coll, key, filename, mime, static_cast<uint64_t>(size_bytes),
                                  sha256, created_ms, chunked, chunk_total});
      }
    }

    // Sort
    if (sort == "size_bytes" || sort == "-size_bytes") {
      std::sort(files.begin(), files.end(), [sort](const FileInfo& a, const FileInfo& b) {
        return sort[0] == '-' ? (a.size_bytes > b.size_bytes) : (a.size_bytes < b.size_bytes);
      });
    } else if (sort == "filename" || sort == "-filename") {
      std::sort(files.begin(), files.end(), [sort](const FileInfo& a, const FileInfo& b) {
        return sort[0] == '-' ? (a.filename > b.filename) : (a.filename < b.filename);
      });
    } else {
      // Default: created_ms desc
      std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.created_ms > b.created_ms;
      });
    }

    // Paginate
    const int64_t total = static_cast<int64_t>(files.size());
    const int64_t start = std::min(offset, total);
    const int64_t end = std::min(start + limit, total);

    JsonValue::Array arr;
    for (int64_t i = start; i < end; ++i) {
      const auto& f = files[i];
      JsonValue::Object obj;
      obj.emplace_back("collection", JsonValue(f.collection));
      obj.emplace_back("key", JsonValue(f.key));
      obj.emplace_back("filename", JsonValue(f.filename));
      obj.emplace_back("mime", JsonValue(f.mime));
      obj.emplace_back("size_bytes", JsonValue(static_cast<int64_t>(f.size_bytes)));
      obj.emplace_back("sha256", JsonValue(f.sha256));
      obj.emplace_back("created_ms", JsonValue(f.created_ms));
      obj.emplace_back("chunked", JsonValue(f.chunked));
      if (f.chunked) {
        obj.emplace_back("chunk_total", JsonValue(f.chunk_total));
      }
      arr.emplace_back(JsonValue(std::move(obj)));
    }

    JsonValue::Object result;
    result.emplace_back("files", JsonValue(std::move(arr)));
    result.emplace_back("total", JsonValue(total));
    result.emplace_back("has_more", JsonValue(end < total));
    return JsonOk(JsonValue(std::move(result)));
  });

  // GET /_files/search - search files by filename/content
  server->Get("/_files/search", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string query = QueryString(req, "q");
    if (query.empty()) {
      return JsonError(400, "missing required query parameter 'q'");
    }

    const std::string collection_filter = QueryString(req, "collection");
    const std::string mime_filter = QueryString(req, "mime");
    const int64_t limit = std::max<int64_t>(1, QueryInt(req, "limit", 100));
    const int64_t offset = std::max<int64_t>(0, QueryInt(req, "offset", 0));

    std::vector<std::string> collections;
    if (collection_filter.empty()) {
      collections = engine->ListCollections();
    } else {
      if (engine->CanRead(collection_filter, self())) {
        collections.push_back(collection_filter);
      }
    }

    struct SearchResult {
      std::string collection;
      std::string key;
      std::string filename;
      std::string mime;
      uint64_t size_bytes;
      std::string sha256;
      int64_t created_ms;
      bool chunked;
      int64_t chunk_total;
      float score;
      bool filename_match;
    };

    std::vector<SearchResult> results;
    const std::string query_lower = query;
    for (const std::string& coll : collections) {
      if (!engine->CanRead(coll, self())) continue;

      const size_t fetch_limit = static_cast<size_t>(limit * 4 + 256);
      auto docs = engine->ListDocuments(coll, "", fetch_limit, self());

      for (auto& [key, doc] : docs) {
        if (!doc.is_object()) continue;

        const JsonValue* _fn = doc.Find("filename");
        const std::string filename = (_fn && _fn->is_string()) ? _fn->AsString() : "";
        if (filename.empty()) continue;

        const JsonValue* _mi = doc.Find("mime");
        const std::string mime = (_mi && _mi->is_string()) ? _mi->AsString() : "application/octet-stream";
        const JsonValue* _sz = doc.Find("size_bytes");
        const int64_t size_bytes = (_sz && _sz->is_number()) ? _sz->AsInt() : 0;
        const JsonValue* _sh = doc.Find("sha256");
        const std::string sha256 = (_sh && _sh->is_string()) ? _sh->AsString() : "";
        const JsonValue* _cm = doc.Find("created_ms");
        const int64_t created_ms = (_cm && _cm->is_number()) ? _cm->AsInt() : 0;
        const JsonValue* _ch = doc.Find("chunked");
        const bool chunked = (_ch && _ch->is_bool()) ? _ch->AsBool() : false;
        const JsonValue* _ct = doc.Find("chunk_total");
        const int64_t chunk_total = (_ct && _ct->is_number()) ? _ct->AsInt() : 0;

        if (!mime_filter.empty() && mime.find(mime_filter) != 0) continue;

        // Search in filename (case-insensitive)
        bool filename_match = false;
        bool content_match = false;

        if (!query.empty()) {
          std::string filename_lower = filename;
          std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (filename_lower.find(query_lower) != std::string::npos) {
            filename_match = true;
          }
        }

        // Optional: search in text_excerpt if present
        const JsonValue* _te = doc.Find("text_excerpt");
        const std::string text_excerpt = (_te && _te->is_string()) ? _te->AsString() : "";
        if (!text_excerpt.empty()) {
          std::string excerpt_lower = text_excerpt;
          std::transform(excerpt_lower.begin(), excerpt_lower.end(), excerpt_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (excerpt_lower.find(query_lower) != std::string::npos) {
            content_match = true;
          }
        }

        if (!filename_match && !content_match) continue;

        float score = 0.0f;
        if (filename_match) score += 10.0f;
        if (content_match) score += 1.0f;

        results.push_back(SearchResult{coll, key, filename, mime, static_cast<uint64_t>(size_bytes),
                                        sha256, created_ms, chunked, chunk_total, score, filename_match});
      }
    }

    // Sort by score desc, then by created_ms desc
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
      if (a.score != b.score) return a.score > b.score;
      return a.created_ms > b.created_ms;
    });

    // Paginate
    const int64_t total = static_cast<int64_t>(results.size());
    const int64_t start = std::min(offset, total);
    const int64_t end = std::min(start + limit, total);

    JsonValue::Array arr;
    for (int64_t i = start; i < end; ++i) {
      const auto& r = results[i];
      JsonValue::Object obj;
      obj.emplace_back("collection", JsonValue(r.collection));
      obj.emplace_back("key", JsonValue(r.key));
      obj.emplace_back("filename", JsonValue(r.filename));
      obj.emplace_back("mime", JsonValue(r.mime));
      obj.emplace_back("size_bytes", JsonValue(static_cast<int64_t>(r.size_bytes)));
      obj.emplace_back("sha256", JsonValue(r.sha256));
      obj.emplace_back("created_ms", JsonValue(r.created_ms));
      obj.emplace_back("chunked", JsonValue(r.chunked));
      if (r.chunked) {
        obj.emplace_back("chunk_total", JsonValue(r.chunk_total));
      }
      obj.emplace_back("score", JsonValue(r.score));
      obj.emplace_back("filename_match", JsonValue(r.filename_match));
      arr.emplace_back(JsonValue(std::move(obj)));
    }

    JsonValue::Object result;
    result.emplace_back("results", JsonValue(std::move(arr)));
    result.emplace_back("total", JsonValue(total));
    result.emplace_back("query", JsonValue(query));
    return JsonOk(JsonValue(std::move(result)));
  });

  // GET /_files/recent - recently created files
  server->Get("/_files/recent", [engine, self](const HttpRequest& req) -> HttpResponse {
    const int64_t limit = std::max<int64_t>(1, QueryInt(req, "limit", 20));
    const std::string collection_filter = QueryString(req, "collection");

    std::vector<std::string> collections;
    if (collection_filter.empty()) {
      collections = engine->ListCollections();
    } else {
      if (engine->CanRead(collection_filter, self())) {
        collections.push_back(collection_filter);
      }
    }

    struct FileInfo {
      std::string collection;
      std::string key;
      std::string filename;
      std::string mime;
      uint64_t size_bytes;
      std::string sha256;
      int64_t created_ms;
      bool chunked;
    };

    std::vector<FileInfo> files;
    for (const std::string& coll : collections) {
      if (!engine->CanRead(coll, self())) continue;

      const size_t fetch_limit = static_cast<size_t>(limit * 2 + 64);
      auto docs = engine->ListDocuments(coll, "", fetch_limit, self());

      for (auto& [key, doc] : docs) {
        if (!doc.is_object()) continue;

        const JsonValue* _fn = doc.Find("filename");
        const std::string filename = (_fn && _fn->is_string()) ? _fn->AsString() : "";
        if (filename.empty()) continue;

        const JsonValue* _mi = doc.Find("mime");
        const std::string mime = (_mi && _mi->is_string()) ? _mi->AsString() : "application/octet-stream";
        const JsonValue* _sz = doc.Find("size_bytes");
        const int64_t size_bytes = (_sz && _sz->is_number()) ? _sz->AsInt() : 0;
        const JsonValue* _sh = doc.Find("sha256");
        const std::string sha256 = (_sh && _sh->is_string()) ? _sh->AsString() : "";
        const JsonValue* _cm = doc.Find("created_ms");
        const int64_t created_ms = (_cm && _cm->is_number()) ? _cm->AsInt() : 0;
        const JsonValue* _ch = doc.Find("chunked");
        const bool chunked = (_ch && _ch->is_bool()) ? _ch->AsBool() : false;

        files.push_back(FileInfo{coll, key, filename, mime, static_cast<uint64_t>(size_bytes),
                                  sha256, created_ms, chunked});
      }
    }

    // Sort by created_ms desc
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
      return a.created_ms > b.created_ms;
    });

    // Take top N
    if (static_cast<int64_t>(files.size()) > limit) {
      files.resize(static_cast<size_t>(limit));
    }

    JsonValue::Array arr;
    for (const auto& f : files) {
      JsonValue::Object obj;
      obj.emplace_back("collection", JsonValue(f.collection));
      obj.emplace_back("key", JsonValue(f.key));
      obj.emplace_back("filename", JsonValue(f.filename));
      obj.emplace_back("mime", JsonValue(f.mime));
      obj.emplace_back("size_bytes", JsonValue(static_cast<int64_t>(f.size_bytes)));
      obj.emplace_back("sha256", JsonValue(f.sha256));
      obj.emplace_back("created_ms", JsonValue(f.created_ms));
      obj.emplace_back("chunked", JsonValue(f.chunked));
      arr.emplace_back(JsonValue(std::move(obj)));
    }

    return JsonOk(JsonValue(std::move(arr)));
  });

  // GET /_files/:collection/:key/stream - stream file content
  server->Get("/_files/:collection/:key/stream", [engine, self](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& key = req.params.at("key");

    if (!engine->CanRead(collection, self())) {
      return JsonError(404, "file not found or not accessible");
    }

    auto doc_or = engine->GetDocument(collection, key, self());
    if (!doc_or.ok()) {
      return StatusError(doc_or.status());
    }

    const JsonValue& doc = doc_or.value();
    if (!doc.is_object()) {
      return JsonError(500, "document is not a valid file object");
    }

    const JsonValue* _mi = doc.Find("mime");
    const std::string mime = (_mi && _mi->is_string()) ? _mi->AsString() : "application/octet-stream";
    const JsonValue* _ch = doc.Find("chunked");
    const bool chunked = (_ch && _ch->is_bool()) ? _ch->AsBool() : false;
    const JsonValue* _sz = doc.Find("size_bytes");
    const int64_t expected_size = (_sz && _sz->is_number()) ? _sz->AsInt() : -1;
    const JsonValue* _sh = doc.Find("sha256");
    const std::string expected_sha256 = (_sh && _sh->is_string()) ? _sh->AsString() : "";

    std::string base64_data;
    if (chunked) {
      auto reassembled = ReassembleChunkedFile(engine, collection, key, doc, self());
      if (!reassembled.ok()) {
        return StatusError(reassembled.status());
      }
      base64_data = std::move(reassembled.value());
    } else {
      const JsonValue* _b64 = doc.Find("base64");
      base64_data = (_b64 && _b64->is_string()) ? _b64->AsString() : "";
    }

    // Decode base64 to binary
    std::vector<uint8_t> binary = DecodeBase64(base64_data);
    if (binary.empty() && expected_size != 0) {
      return JsonError(500, "failed to decode file content");
    }
    if (expected_size >= 0 && static_cast<int64_t>(binary.size()) != expected_size) {
      return JsonError(422, "file content size does not match its manifest");
    }
    if (!expected_sha256.empty()) {
      if (expected_sha256.size() != 64) {
        return JsonError(422, "file manifest contains an invalid SHA-256 checksum");
      }
      const std::string actual_sha256 = HexEncode(crypto::Sha256(
          std::string(reinterpret_cast<const char*>(binary.data()), binary.size())));
      if (actual_sha256 != expected_sha256) {
        return JsonError(422, "file content checksum does not match its manifest");
      }
    }

    HttpResponse resp;
    resp.status = 200;
    resp.content_type = mime;
    resp.body.assign(reinterpret_cast<const char*>(binary.data()), binary.size());
    return resp;
  });
}

}  // namespace desentry
