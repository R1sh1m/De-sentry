#pragma once
// Offline plaintext -> sealed migration (`desentryd --re-encrypt --config ...`).
//
// The node must be STOPPED first: files are rewritten in place (tmp + rename
// per file), and a running node would keep writing plaintext behind the
// rewrite. Every file this touches is one the sealed open paths understand:
// paged `*.dsf` files (4096B pages -> 4124B sealed pages + a `.enc` sidecar
// header), length-framed logs (desentry.wal, transit.log, outbox.log,
// cross_engine_index.log -- each record payload sealed), whole-file JSON
// (catalog.json, roots.json, engine manifests) and identity.key.
//
// Refuses rather than half-migrates: any torn file it cannot interpret
// (partial `.dsf` page, undecodable JSON), any node.json listing a vendored
// engine (their files are outside the sealed layer), and any I/O failure all
// abort with the file named. Files already sealed are skipped, so the tool
// is idempotent: re-running it after an abort finishes the job, sealing what
// is still plaintext. node.json, manifest.json, ledger_migration.json and
// managed_nodes.json are never touched (config/audit metadata, non-secret).
//
// Encrypted -> plaintext is deliberately unsupported (restore from backup);
// the tool only ever moves toward sealed.

#include <string>

#include "desentry/common/status.h"

namespace desentry {

// Seals every eligible plaintext file under `data_dir` with `dek` (32 raw
// bytes). `summary` receives a human-readable "N files sealed" line on
// success for the tool's stdout.
Status ReencryptDataDir(const std::string& data_dir, const std::string& dek,
                        std::string* summary);

}  // namespace desentry
