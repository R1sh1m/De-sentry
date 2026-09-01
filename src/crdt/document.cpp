#include "desentry/crdt/document.h"

#include <atomic>

#include "desentry/common/byte_buffer.h"

namespace desentry {

namespace {
enum class WireKind : uint8_t { kNull = 0, kBool = 1, kInt = 2, kDouble = 3, kString = 4, kArray = 5, kObject = 6 };
}  // namespace

std::string CrdtValue::NextTag(const HLCTimestamp& ts) {
  static std::atomic<uint64_t> counter{0};
  uint64_t seq = counter.fetch_add(1);
  return ts.node_id + ":" + std::to_string(ts.physical_ms) + "." + std::to_string(ts.logical) +
         "#" + std::to_string(seq);
}

CrdtValue CrdtValue::MakeTombstone(const HLCTimestamp& ts) {
  CrdtValue v;
  v.kind_ = Kind::kNull;
  v.ts_ = ts;
  v.tombstone_ = true;
  return v;
}

bool CrdtValue::JsonEquals(const JsonValue& a, const JsonValue& b) {
  if (a.type() != b.type()) {
    // Treat int/double as comparable numerically so "5" and "5.0" aren't
    // spuriously flagged as a change.
    if (a.is_number() && b.is_number()) return a.AsDouble() == b.AsDouble();
    return false;
  }
  switch (a.type()) {
    case JsonType::kNull: return true;
    case JsonType::kBool: return a.AsBool() == b.AsBool();
    case JsonType::kInt: return a.AsInt() == b.AsInt();
    case JsonType::kDouble: return a.AsDouble() == b.AsDouble();
    case JsonType::kString: return a.AsString() == b.AsString();
    case JsonType::kArray: {
      const auto& av = a.AsArray();
      const auto& bv = b.AsArray();
      if (av.size() != bv.size()) return false;
      for (size_t i = 0; i < av.size(); ++i) {
        if (!JsonEquals(av[i], bv[i])) return false;
      }
      return true;
    }
    case JsonType::kObject: {
      const auto& ao = a.AsObject();
      const auto& bo = b.AsObject();
      if (ao.size() != bo.size()) return false;
      for (auto& [k, v] : ao) {
        const JsonValue* other = b.Find(k);
        if (!other || !JsonEquals(v, *other)) return false;
      }
      return true;
    }
  }
  return false;
}

CrdtValue CrdtValue::FromJson(const JsonValue& json, const HLCTimestamp& ts) {
  CrdtValue v;
  v.ts_ = ts;
  switch (json.type()) {
    case JsonType::kNull: v.kind_ = Kind::kNull; return v;
    case JsonType::kBool: v.kind_ = Kind::kBool; v.bool_v_ = json.AsBool(); return v;
    case JsonType::kInt: v.kind_ = Kind::kInt; v.int_v_ = json.AsInt(); return v;
    case JsonType::kDouble: v.kind_ = Kind::kDouble; v.double_v_ = json.AsDouble(); return v;
    case JsonType::kString: v.kind_ = Kind::kString; v.str_v_ = json.AsString(); return v;
    case JsonType::kArray: {
      v.kind_ = Kind::kArray;
      for (auto& elem : json.AsArray()) {
        ArrayElem ae;
        ae.tag = NextTag(ts);
        ae.value = FromJson(elem, ts);
        ae.tombstone = false;
        v.array_v_.push_back(std::move(ae));
      }
      return v;
    }
    case JsonType::kObject: {
      v.kind_ = Kind::kObject;
      for (auto& [k, val] : json.AsObject()) {
        v.object_v_.emplace_back(k, FromJson(val, ts));
      }
      return v;
    }
  }
  return v;
}

CrdtValue CrdtValue::ApplyJsonUpdate(const CrdtValue& previous, const JsonValue& new_json,
                                      const HLCTimestamp& ts) {
  if (!previous.tombstone_ && JsonEquals(previous.ToJson(), new_json)) {
    return previous;  // no-op: preserve existing tags/timestamps, no churn.
  }

  if (previous.kind_ == Kind::kObject && new_json.is_object() && !previous.tombstone_) {
    CrdtValue result;
    result.kind_ = Kind::kObject;
    result.ts_ = ts;

    // Fields present in the new document: recurse (or keep unchanged).
    for (auto& [key, new_child] : new_json.AsObject()) {
      const CrdtValue* prev_child = nullptr;
      for (auto& kv : previous.object_v_) {
        if (kv.first == key) { prev_child = &kv.second; break; }
      }
      CrdtValue merged_child = prev_child ? ApplyJsonUpdate(*prev_child, new_child, ts)
                                          : FromJson(new_child, ts);
      result.object_v_.emplace_back(key, std::move(merged_child));
    }
    // Fields present in the old document but missing from the new one:
    // tombstone them (this is also how whole-document delete works, by
    // calling this with new_json == an empty object).
    for (auto& [key, prev_child] : previous.object_v_) {
      if (new_json.Has(key)) continue;  // already handled above
      if (prev_child.tombstone_) {
        result.object_v_.emplace_back(key, prev_child);  // already gone, carry forward
      } else {
        result.object_v_.emplace_back(key, MakeTombstone(ts));
      }
    }
    return result;
  }

  if (previous.kind_ == Kind::kArray && new_json.is_array() && !previous.tombstone_) {
    CrdtValue result;
    result.kind_ = Kind::kArray;
    result.ts_ = ts;

    std::vector<bool> matched(previous.array_v_.size(), false);
    for (auto& new_elem_json : new_json.AsArray()) {
      bool found = false;
      for (size_t i = 0; i < previous.array_v_.size(); ++i) {
        if (matched[i] || previous.array_v_[i].tombstone) continue;
        if (JsonEquals(previous.array_v_[i].value.ToJson(), new_elem_json)) {
          result.array_v_.push_back(previous.array_v_[i]);  // unchanged: keep tag+ts
          matched[i] = true;
          found = true;
          break;
        }
      }
      if (!found) {
        ArrayElem ae;
        ae.tag = NextTag(ts);
        ae.value = FromJson(new_elem_json, ts);
        ae.tombstone = false;
        result.array_v_.push_back(std::move(ae));
      }
    }
    // Carry forward every previous element that wasn't matched: tombstone
    // the ones that were live (removed by this write), keep already-dead
    // ones as-is. OR-Set correctness requires never forgetting a tag once
    // it has been seen, even after removal.
    for (size_t i = 0; i < previous.array_v_.size(); ++i) {
      if (matched[i]) continue;
      ArrayElem ae = previous.array_v_[i];
      ae.tombstone = true;
      result.array_v_.push_back(std::move(ae));
    }
    return result;
  }

  // Structural replace: scalar changed value/type, kind changed entirely,
  // or the field is being resurrected after a prior delete.
  return FromJson(new_json, ts);
}

CrdtValue CrdtValue::Merge(const CrdtValue& a, const CrdtValue& b) {
  // A tombstone (a delete, at any nesting level) is resolved purely by
  // LWW against whatever the other side has, regardless of the other
  // side's kind -- this is what lets a delete correctly beat or lose to a
  // concurrent update at every level of the document, not just the root.
  if (a.tombstone_ || b.tombstone_) {
    return (b.ts_ > a.ts_) ? b : a;
  }

  if (a.kind_ == Kind::kObject && b.kind_ == Kind::kObject) {
    CrdtValue result;
    result.kind_ = Kind::kObject;
    result.ts_ = (b.ts_ > a.ts_) ? b.ts_ : a.ts_;

    for (auto& [key, av] : a.object_v_) {
      const CrdtValue* bv = nullptr;
      for (auto& kv : b.object_v_) {
        if (kv.first == key) { bv = &kv.second; break; }
      }
      result.object_v_.emplace_back(key, bv ? Merge(av, *bv) : av);
    }
    for (auto& [key, bv] : b.object_v_) {
      bool seen = false;
      for (auto& kv : a.object_v_) {
        if (kv.first == key) { seen = true; break; }
      }
      if (!seen) result.object_v_.emplace_back(key, bv);
    }
    return result;
  }

  if (a.kind_ == Kind::kArray && b.kind_ == Kind::kArray) {
    CrdtValue result;
    result.kind_ = Kind::kArray;
    result.ts_ = (b.ts_ > a.ts_) ? b.ts_ : a.ts_;

    for (auto& ae : a.array_v_) {
      const ArrayElem* be = nullptr;
      for (auto& e : b.array_v_) {
        if (e.tag == ae.tag) { be = &e; break; }
      }
      if (be) {
        ArrayElem merged;
        merged.tag = ae.tag;
        merged.value = Merge(ae.value, be->value);
        merged.tombstone = ae.tombstone || be->tombstone;  // sticky removal
        result.array_v_.push_back(std::move(merged));
      } else {
        result.array_v_.push_back(ae);
      }
    }
    for (auto& be : b.array_v_) {
      bool seen = false;
      for (auto& e : a.array_v_) {
        if (e.tag == be.tag) { seen = true; break; }
      }
      if (!seen) result.array_v_.push_back(be);
    }
    return result;
  }

  // Scalar-vs-scalar, or a structural conflict (concurrent writes gave this
  // field different *kinds*, e.g. one peer wrote a number where another
  // wrote an object): resolve as whole-node LWW. The loser is discarded
  // entirely rather than merged piecewise, which is the only sound thing to
  // do when the two sides don't even agree on the field's shape.
  return (b.ts_ > a.ts_) ? b : a;
}

JsonValue CrdtValue::ToJson() const {
  if (tombstone_) return JsonValue(nullptr);
  switch (kind_) {
    case Kind::kNull: return JsonValue(nullptr);
    case Kind::kBool: return JsonValue(bool_v_);
    case Kind::kInt: return JsonValue(int_v_);
    case Kind::kDouble: return JsonValue(double_v_);
    case Kind::kString: return JsonValue(str_v_);
    case Kind::kArray: {
      JsonValue::Array arr;
      for (auto& e : array_v_) {
        if (!e.tombstone) arr.push_back(e.value.ToJson());
      }
      return JsonValue(std::move(arr));
    }
    case Kind::kObject: {
      JsonValue::Object obj;
      for (auto& [k, v] : object_v_) {
        if (!v.tombstone_) obj.emplace_back(k, v.ToJson());
      }
      return JsonValue(std::move(obj));
    }
  }
  return JsonValue(nullptr);
}

std::string CrdtValue::Encode() const {
  ByteWriter w;
  WireKind wk;
  switch (kind_) {
    case Kind::kNull: wk = WireKind::kNull; break;
    case Kind::kBool: wk = WireKind::kBool; break;
    case Kind::kInt: wk = WireKind::kInt; break;
    case Kind::kDouble: wk = WireKind::kDouble; break;
    case Kind::kString: wk = WireKind::kString; break;
    case Kind::kArray: wk = WireKind::kArray; break;
    case Kind::kObject: wk = WireKind::kObject; break;
    default: wk = WireKind::kNull; break;
  }
  w.U8(static_cast<uint8_t>(wk));
  w.U8(tombstone_ ? 1 : 0);
  w.Bytes(ts_.Encode());

  switch (kind_) {
    case Kind::kNull: break;
    case Kind::kBool: w.U8(bool_v_ ? 1 : 0); break;
    case Kind::kInt: w.I64(int_v_); break;
    case Kind::kDouble: w.F64(double_v_); break;
    case Kind::kString: w.Bytes(str_v_); break;
    case Kind::kArray: {
      w.U32(static_cast<uint32_t>(array_v_.size()));
      for (auto& e : array_v_) {
        w.Bytes(e.tag);
        w.U8(e.tombstone ? 1 : 0);
        w.Bytes(e.value.Encode());
      }
      break;
    }
    case Kind::kObject: {
      w.U32(static_cast<uint32_t>(object_v_.size()));
      for (auto& [k, v] : object_v_) {
        w.Bytes(k);
        w.Bytes(v.Encode());
      }
      break;
    }
  }
  return w.TakeString();
}

CrdtValue CrdtValue::Decode(const std::string& bytes) {
  ByteReader r(bytes);
  CrdtValue v;
  auto wk = static_cast<WireKind>(r.U8());
  v.tombstone_ = r.U8() != 0;
  v.ts_ = HLCTimestamp::Decode(r.Bytes());

  switch (wk) {
    case WireKind::kNull: v.kind_ = Kind::kNull; break;
    case WireKind::kBool: v.kind_ = Kind::kBool; v.bool_v_ = r.U8() != 0; break;
    case WireKind::kInt: v.kind_ = Kind::kInt; v.int_v_ = r.I64(); break;
    case WireKind::kDouble: v.kind_ = Kind::kDouble; v.double_v_ = r.F64(); break;
    case WireKind::kString: v.kind_ = Kind::kString; v.str_v_ = r.Bytes(); break;
    case WireKind::kArray: {
      v.kind_ = Kind::kArray;
      uint32_t n = r.U32();
      v.array_v_.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        CrdtArrayElem e;
        e.tag = r.Bytes();
        e.tombstone = r.U8() != 0;
        e.value = Decode(r.Bytes());
        v.array_v_.push_back(std::move(e));
      }
      break;
    }
    case WireKind::kObject: {
      v.kind_ = Kind::kObject;
      uint32_t n = r.U32();
      v.object_v_.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::string k = r.Bytes();
        CrdtValue child = Decode(r.Bytes());
        v.object_v_.emplace_back(std::move(k), std::move(child));
      }
      break;
    }
  }
  return v;
}

HLCTimestamp CrdtValue::MaxTimestamp() const {
  HLCTimestamp best = ts_;
  if (kind_ == Kind::kArray) {
    for (auto& e : array_v_) {
      HLCTimestamp child_max = e.value.MaxTimestamp();
      if (child_max > best) best = child_max;
    }
  } else if (kind_ == Kind::kObject) {
    for (auto& [k, v] : object_v_) {
      HLCTimestamp child_max = v.MaxTimestamp();
      if (child_max > best) best = child_max;
    }
  }
  return best;
}

bool CrdtValue::IsEmpty() const {
  if (kind_ != Kind::kObject) return tombstone_;
  for (auto& [k, v] : object_v_) {
    if (!v.tombstone_) return false;
  }
  return true;
}

}  // namespace desentry
