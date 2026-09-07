#pragma once
// Adapters for the vendored OSS backbone.
//
// Every backend declared here is compiled **only** when its amalgamation has
// been dropped into third_party/ and the matching CMake option is on:
//
//   DESENTRY_WITH_SQLITE      -> sqlite  (public domain)      relational/metadata core, JSON1, FTS5
//   DESENTRY_WITH_SQLITE_VEC  -> sqlite_vec (Apache-2.0/MIT)  vector search over the SQLite core
//   DESENTRY_WITH_DUCKDB      -> duckdb  (MIT)                columnar / analytical
//   DESENTRY_WITH_LMDB        -> lmdb    (OpenLDAP licence)   generic + transit KV
//
// With none of them present the tree still builds and every workload is
// served by the from-scratch family (kv, columnar_lite, ts_rollup,
// vector_hnsw_lite, graph_adj). That is not a fallback of convenience: the
// product ships as an offline desktop app with zero fetched dependencies at
// runtime, so "works with nothing vendored" is the shipping configuration
// and the vendored backbone is the opt-in upgrade for deployments that want
// SQL, FTS5 or DuckDB's analytical engine.
//
// MakeBackend() in storage/router.cpp consults these at construction time
// and returns kInvalidArgument for a name whose backend was not compiled in,
// naming the CMake option -- so a config asking for "duckdb" on a build
// without it fails with an actionable message rather than silently falling
// back to a different storage layout.
//
// Licence discipline (deliberate, and enforced by review not just prose):
// nothing GPL (no Xapian) or AGPL (no TDengine) enters this tree, and
// nothing that requires a separate server process (no ClickHouse, no
// QuestDB) -- a server dependency would break the one-app, no-daemon
// install this product is built around.

#include <memory>
#include <string>

#include "desentry/common/status.h"
#include "desentry/storage/engines/engine_common.h"

namespace desentry {

// True when the named engine was compiled into this binary. The app's
// creation wizard calls this (via GET /_engines) so the engine picker only
// offers backends that actually exist here.
bool VendoredBackendAvailable(const std::string& engine_name);

// Returns the CMake option that would enable `engine_name`, for error text.
std::string VendoredBackendOption(const std::string& engine_name);

// Constructs one vendored backend, or kInvalidArgument if it is not
// compiled in. Never returns nullptr with an OK status.
StatusOr<std::unique_ptr<EngineBackend>> MakeVendoredBackend(const std::string& engine_name);

}  // namespace desentry
