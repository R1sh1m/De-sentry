#pragma once
// Registers every local REST endpoint an application uses to talk to its
// own peer (ARCHITECTURE.md §3.1) against a running HttpServer.

#include "desentry/api/http_server.h"
#include "desentry/engine/node_engine.h"
#include "desentry/net/network_manager.h"

namespace desentry {

void RegisterRoutes(HttpServer* server, NodeEngine* engine, NetworkManager* network);

// Validates `doc` against a JSON-Schema *subset* (top-level
// {"type":"object","properties":{name:{"type":...}},"required":[...]}) --
// enough to give "structured" collections real validation without
// building a full JSON Schema implementation. Returns OK or an
// InvalidArgument/SchemaViolation Status describing the first failure.
Status ValidateAgainstSchema(const JsonValue& doc, const JsonValue& schema);

}  // namespace desentry
