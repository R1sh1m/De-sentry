#include "desentry/api/routes.h"

#include <algorithm>
#include <chrono>

#include "desentry/common/hex.h"
#include "desentry/common/logger.h"

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
    case StatusCode::kAuthError: return 401;
    case StatusCode::kOutOfSpace: return 507;
    default: return 500;
  }
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

void RegisterRoutes(HttpServer* server, NodeEngine* engine, NetworkManager* network) {
  static auto start_time = std::chrono::steady_clock::now();

  // -- document CRUD --------------------------------------------------------
  server->Put("/db/:collection/:key", [engine](const HttpRequest& req) -> HttpResponse {
    const std::string& collection = req.params.at("collection");
    const std::string& key = req.params.at("key");

    JsonValue body;
    try {
      body = JsonValue::Parse(req.body.empty() ? "{}" : req.body);
    } catch (const std::exception& e) {
      return JsonError(400, std::string("invalid JSON body: ") + e.what());
    }

    const CollectionMeta* meta = engine->storage().catalog().Get(collection);
    if (meta != nullptr && meta->has_schema) {
      Status schema_st = ValidateAgainstSchema(body, meta->schema);
      if (!schema_st.ok()) return JsonError(StatusToHttpCode(schema_st), schema_st.message());
    }

    Status st = engine->PutDocument(collection, key, body);
    if (!st.ok()) return JsonError(StatusToHttpCode(st), st.message());

    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    obj.emplace_back("collection", JsonValue(collection));
    obj.emplace_back("key", JsonValue(key));
    return JsonOk(JsonValue(std::move(obj)), 200);
  });

  server->Get("/db/:collection/:key", [engine](const HttpRequest& req) -> HttpResponse {
    auto doc_or = engine->GetDocument(req.params.at("collection"), req.params.at("key"));
    if (!doc_or.ok()) return JsonError(StatusToHttpCode(doc_or.status()), doc_or.status().message());
    return JsonOk(doc_or.value());
  });

  server->Del("/db/:collection/:key", [engine](const HttpRequest& req) -> HttpResponse {
    Status st = engine->DeleteDocument(req.params.at("collection"), req.params.at("key"));
    if (!st.ok()) return JsonError(StatusToHttpCode(st), st.message());
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/db/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    std::string start_key;
    size_t limit = 100;
    auto sk_it = req.query.find("start_key");
    if (sk_it != req.query.end()) start_key = sk_it->second;
    auto lim_it = req.query.find("limit");
    if (lim_it != req.query.end()) {
      try { limit = static_cast<size_t>(std::stoul(lim_it->second)); } catch (...) {}
    }

    auto docs = engine->ListDocuments(req.params.at("collection"), start_key, limit);
    JsonValue::Array arr;
    for (auto& [key, doc] : docs) {
      JsonValue::Object entry;
      entry.emplace_back("key", JsonValue(key));
      entry.emplace_back("document", doc);
      arr.emplace_back(std::move(entry));
    }
    JsonValue::Object obj;
    obj.emplace_back("collection", JsonValue(req.params.at("collection")));
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
    if (!st.ok()) return JsonError(StatusToHttpCode(st), st.message());
    JsonValue::Object obj;
    obj.emplace_back("ok", JsonValue(true));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_schema/:collection", [engine](const HttpRequest& req) -> HttpResponse {
    const CollectionMeta* meta = engine->storage().catalog().Get(req.params.at("collection"));
    if (meta == nullptr || !meta->has_schema) return JsonOk(JsonValue(nullptr));
    return JsonOk(meta->schema);
  });

  // -- collections / cluster introspection ------------------------------------
  server->Get("/_collections", [engine](const HttpRequest&) -> HttpResponse {
    JsonValue::Array arr;
    for (auto& name : engine->ListCollections()) arr.emplace_back(JsonValue(name));
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Get("/_peers", [network](const HttpRequest&) -> HttpResponse {
    JsonValue::Array arr;
    for (auto& p : network->peers().List()) {
      JsonValue::Object obj;
      obj.emplace_back("node_id", JsonValue(p.node_id));
      obj.emplace_back("host", JsonValue(p.host));
      obj.emplace_back("p2p_port", JsonValue(static_cast<int64_t>(p.p2p_port)));
      obj.emplace_back("last_seen_ms", JsonValue(static_cast<int64_t>(p.last_seen_ms)));
      arr.emplace_back(std::move(obj));
    }
    return JsonOk(JsonValue(std::move(arr)));
  });

  server->Get("/_status", [engine, network](const HttpRequest&) -> HttpResponse {
    auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("public_key_hex", JsonValue(HexEncode(engine->identity().public_key())));
    obj.emplace_back("uptime_seconds", JsonValue(static_cast<int64_t>(uptime_s)));
    obj.emplace_back("collections", JsonValue(static_cast<int64_t>(engine->ListCollections().size())));
    obj.emplace_back("known_peers", JsonValue(static_cast<int64_t>(network->peers().Size())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- hash-chained audit ledger ----------------------------------------------
  // See storage/wal.h and NodeEngine::SignLedgerTip() for the design: every
  // WAL record chains from the previous one via SHA-256 (tamper-evidence),
  // and the current tip is additionally signed with this node's Ed25519
  // identity key (origin authenticity -- a peer that trusts this node_id
  // can verify *this node* attests to *this exact* ledger state).
  server->Get("/_ledger/tip", [engine](const HttpRequest&) -> HttpResponse {
    auto tip = engine->LedgerTip();
    std::string signature = engine->SignLedgerTip();
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("entry_id", JsonValue(static_cast<int64_t>(tip.entry_id)));
    obj.emplace_back("entry_hash", JsonValue(HexEncode(tip.entry_hash)));
    obj.emplace_back("signature", JsonValue(HexEncode(signature)));
    obj.emplace_back("public_key_hex", JsonValue(HexEncode(engine->identity().public_key())));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Get("/_ledger/entries", [engine](const HttpRequest& req) -> HttpResponse {
    lsn_t from = 0, to = -1;
    auto from_it = req.query.find("from");
    auto to_it = req.query.find("to");
    try {
      if (from_it != req.query.end()) from = static_cast<lsn_t>(std::stoll(from_it->second));
      if (to_it != req.query.end()) to = static_cast<lsn_t>(std::stoll(to_it->second));
    } catch (...) {
      return JsonError(400, "from/to must be integers");
    }
    lsn_t tip_id = engine->LedgerTip().entry_id;
    if (to < 0 || to > tip_id) to = tip_id;
    // Cap the range returned in a single call -- this is a debug/audit
    // endpoint, not a bulk-export path; a peer wanting a full replay pages
    // through it, same spirit as LEDGER_REPLAY in the comparison design.
    constexpr int64_t kMaxRange = 5000;
    if (to - from + 1 > kMaxRange) to = from + kMaxRange - 1;

    auto entries_or = engine->LedgerEntries(from, to);
    if (!entries_or.ok()) return JsonError(500, entries_or.status().message());

    JsonValue::Array arr;
    for (auto& rec : entries_or.value()) {
      JsonValue::Object e;
      e.emplace_back("entry_id", JsonValue(static_cast<int64_t>(rec.lsn)));
      e.emplace_back("operation", JsonValue(rec.type == WalRecordType::kPut ? "PUT" : rec.type == WalRecordType::kDelete ? "DELETE" : "CHECKPOINT"));
      e.emplace_back("collection", JsonValue(rec.collection));
      e.emplace_back("key", JsonValue(rec.key));
      e.emplace_back("document_bytes", JsonValue(static_cast<int64_t>(rec.document_bytes.size())));
      e.emplace_back("prev_hash", JsonValue(HexEncode(rec.prev_hash)));
      e.emplace_back("entry_hash", JsonValue(HexEncode(rec.entry_hash)));
      arr.emplace_back(std::move(e));
    }
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("count", JsonValue(static_cast<int64_t>(arr.size())));
    obj.emplace_back("entries", JsonValue(std::move(arr)));
    return JsonOk(JsonValue(std::move(obj)));
  });

  server->Post("/_ledger/verify", [engine](const HttpRequest&) -> HttpResponse {
    auto result = engine->VerifyLedger();
    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("verified", JsonValue(result.ok));
    obj.emplace_back("entries_checked", JsonValue(static_cast<int64_t>(result.entries_checked)));
    if (!result.ok) {
      obj.emplace_back("failed_at_entry_id", JsonValue(static_cast<int64_t>(result.failed_at_entry_id)));
      obj.emplace_back("reason", JsonValue(result.reason));
    }
    return JsonOk(JsonValue(std::move(obj)));
  });

  // -- brain file: a compact, human/agent-readable snapshot of this node's
  // entire state, so a peer (or an AI agent orchestrating the consortium)
  // can get full network-wide awareness without running expensive queries
  // against every node. See ARCHITECTURE.md's ledger/brain-file section.
  server->Get("/_brain", [engine, network](const HttpRequest&) -> HttpResponse {
    auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
    auto tip = engine->LedgerTip();

    JsonValue::Array collections;
    for (auto& name : engine->ListCollections()) {
      auto summary = engine->Summarize(name);
      JsonValue::Object c;
      c.emplace_back("name", JsonValue(summary.name));
      c.emplace_back("document_count", JsonValue(static_cast<int64_t>(summary.document_count)));
      c.emplace_back("checksum", JsonValue(summary.checksum_hex));
      collections.emplace_back(std::move(c));
    }

    JsonValue::Array peers;
    for (auto& p : network->peers().List()) {
      JsonValue::Object po;
      po.emplace_back("node_id", JsonValue(p.node_id));
      po.emplace_back("host", JsonValue(p.host));
      peers.emplace_back(std::move(po));
    }

    JsonValue::Object ledger_tip;
    ledger_tip.emplace_back("entry_id", JsonValue(static_cast<int64_t>(tip.entry_id)));
    ledger_tip.emplace_back("entry_hash", JsonValue(HexEncode(tip.entry_hash)));

    JsonValue::Object obj;
    obj.emplace_back("node_id", JsonValue(engine->identity().node_id()));
    obj.emplace_back("generated_at_us", JsonValue(static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count())));
    obj.emplace_back("uptime_seconds", JsonValue(static_cast<int64_t>(uptime_s)));
    obj.emplace_back("ledger_tip", JsonValue(std::move(ledger_tip)));
    obj.emplace_back("collections", JsonValue(std::move(collections)));
    obj.emplace_back("known_peers", JsonValue(std::move(peers)));
    return JsonOk(JsonValue(std::move(obj)));
  });
}

}  // namespace desentry
