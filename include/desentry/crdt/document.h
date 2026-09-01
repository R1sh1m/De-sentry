#pragma once
// The core conflict-free data type of the whole engine.
//
// CrdtValue is a recursive tree that mirrors JsonValue's shape (null, bool,
// int, double, string, array, object) but every node additionally carries
// an HLC timestamp, and every scalar/array-element carries enough metadata
// to merge deterministically with any other version of the same logical
// value, from any peer, in any order:
//
//   * scalars & the document-as-a-whole -> LWW-Register: highest (ts, node_id) wins.
//   * arrays / sets                     -> OR-Set: union of uniquely-tagged
//                                          elements; once a tag is removed it
//                                          stays removed forever (tombstone is
//                                          monotonic -- this is what lets a
//                                          concurrent add survive a remove that
//                                          didn't observe it).
//   * objects                           -> recursive per-key LWW-map: union
//                                          of keys, each key's value merged
//                                          independently.
//
// Merge is commutative, associative and idempotent for every node kind here
// (a product of CRDTs is itself a CRDT), which is the formal reason two
// peers that have seen the same set of writes always converge to the exact
// same document regardless of what order they received them in.
//
// Structured ("schema'd") and unstructured collections use *exactly* this
// same type -- a schema is a validation rule applied at the API boundary
// before a JSON write reaches ApplyJsonUpdate(), not a different storage
// representation.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "desentry/common/json.h"
#include "desentry/crdt/hlc.h"

namespace desentry {

class CrdtValue;

// Forward-declared here (and fully defined just below CrdtValue) because it
// holds a CrdtValue directly by value -- unlike the vector<...> members
// inside CrdtValue itself (which the standard allows to reference an
// incomplete element type), a *direct* struct member requires the type to
// be complete at the point of definition, so this struct cannot be both
// nested in and fully defined inside CrdtValue's own (still-incomplete)
// class body.
struct CrdtArrayElem;

class CrdtValue {
 public:
  enum class Kind { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

  using ArrayElem = CrdtArrayElem;
  using ObjectFields = std::vector<std::pair<std::string, CrdtValue>>;

  CrdtValue() : kind_(Kind::kNull) {}

  Kind kind() const { return kind_; }
  const HLCTimestamp& ts() const { return ts_; }
  bool tombstone() const { return tombstone_; }

  // -- construction ----------------------------------------------------
  // Builds a brand-new CRDT tree from a plain JSON value with no prior
  // state to diff against (used the very first time a key is written).
  static CrdtValue FromJson(const JsonValue& json, const HLCTimestamp& ts);

  // Diffs `new_json` against `previous`'s currently-visible shape to
  // produce a new CRDT tree carrying the right adds/removes/updates, all
  // stamped with `ts`. This is how a *local* write becomes a set of CRDT
  // operations: unchanged sub-trees keep their old tags/timestamps (so an
  // untouched field never "wins" a merge race it wasn't part of), changed
  // scalars get fresh LWW timestamps, array elements no longer present get
  // OR-Set-tombstoned, new array elements get fresh tags, and object keys
  // missing from `new_json` are tombstoned (this is also how whole-document
  // delete works: call with new_json == empty object).
  static CrdtValue ApplyJsonUpdate(const CrdtValue& previous, const JsonValue& new_json,
                                    const HLCTimestamp& ts);

  // -- merge -------------------------------------------------------------
  static CrdtValue Merge(const CrdtValue& a, const CrdtValue& b);

  // -- materialization -----------------------------------------------------
  // Produces the plain JSON currently visible (tombstones and removed
  // OR-Set elements are dropped).
  JsonValue ToJson() const;

  // Whether this node currently has any live (non-tombstoned) content --
  // used by the engine to decide whether a document is "deleted" (every
  // top-level field tombstoned) for the purposes of API responses / scans.
  bool IsEmpty() const;

  // -- binary codec --------------------------------------------------------
  // Compact binary encoding of the *full* CRDT tree (unlike ToJson(), this
  // round-trips tombstones, OR-Set tags, and per-field timestamps -- it is
  // what gets written to a storage-engine record and what gets shipped to
  // other peers on the wire, since replication and durability share this
  // exact representation (see storage/wal.h's header comment).
  std::string Encode() const;
  static CrdtValue Decode(const std::string& bytes);

  // The newest HLC timestamp anywhere in this tree (recursively) -- used as
  // a cheap, purely-advisory "did this document possibly change" signal in
  // gossip digests (net/wire_protocol.h's DigestPayload). Advisory only:
  // Merge() is always run on the actual document regardless, so an
  // imprecise digest can cost bandwidth but never causes incorrect
  // convergence (see ARCHITECTURE.md §7.5).
  HLCTimestamp MaxTimestamp() const;

 private:
  static std::string NextTag(const HLCTimestamp& ts);
  static bool JsonEquals(const JsonValue& a, const JsonValue& b);
  static CrdtValue MakeTombstone(const HLCTimestamp& ts);

  Kind kind_;
  HLCTimestamp ts_;
  bool tombstone_ = false;

  bool bool_v_ = false;
  int64_t int_v_ = 0;
  double double_v_ = 0.0;
  std::string str_v_;
  std::vector<ArrayElem> array_v_;
  ObjectFields object_v_;
};

struct CrdtArrayElem {
  std::string tag;  // globally-unique add-identifier: node_id + HLC + seq
  CrdtValue value;
  bool tombstone = false;
};

}  // namespace desentry
