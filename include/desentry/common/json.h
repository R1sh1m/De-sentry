#pragma once
// A small, dependency-free JSON value type + parser/serializer.
//
// Why hand-rolled instead of vendoring nlohmann/json: the MVP must build
// fully offline with zero fetched third-party sources (see README "Build
// philosophy"), and this Value type doubles as the in-memory representation
// the CRDT document layer merges over -- we want full control of it rather
// than adapting someone else's object model.
//
// This is intentionally a *textual* JSON parser used at the API boundary
// (HTTP request/response bodies, config files). The on-disk / on-wire
// representation of documents uses a separate compact binary codec
// (see storage/document_codec.h) for space and speed; Value is the bridge
// between the two.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace desentry {

enum class JsonType { kNull, kBool, kInt, kDouble, kString, kArray, kObject };

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  // Ordered (insertion-order) key/value pairs -- JSON objects are not
  // inherently sorted, and preserving input order makes round-tripping and
  // human debugging saner. Canonical (sorted) form is available via
  // CanonicalDump() for anything that must hash/sign deterministically.
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() : type_(JsonType::kNull) {}
  JsonValue(std::nullptr_t) : type_(JsonType::kNull) {}                 // NOLINT
  JsonValue(bool b) : type_(JsonType::kBool), bool_(b) {}               // NOLINT
  JsonValue(int64_t i) : type_(JsonType::kInt), int_(i) {}              // NOLINT
  JsonValue(int i) : type_(JsonType::kInt), int_(i) {}                  // NOLINT
  JsonValue(double d) : type_(JsonType::kDouble), double_(d) {}         // NOLINT
  JsonValue(std::string s) : type_(JsonType::kString), str_(std::move(s)) {}  // NOLINT
  JsonValue(const char* s) : type_(JsonType::kString), str_(s) {}       // NOLINT
  JsonValue(Array a) : type_(JsonType::kArray), arr_(std::move(a)) {}   // NOLINT
  JsonValue(Object o) : type_(JsonType::kObject), obj_(std::move(o)) {} // NOLINT

  static JsonValue MakeArray() { JsonValue v; v.type_ = JsonType::kArray; return v; }
  static JsonValue MakeObject() { JsonValue v; v.type_ = JsonType::kObject; return v; }

  JsonType type() const { return type_; }
  bool is_null() const { return type_ == JsonType::kNull; }
  bool is_bool() const { return type_ == JsonType::kBool; }
  bool is_int() const { return type_ == JsonType::kInt; }
  bool is_double() const { return type_ == JsonType::kDouble; }
  bool is_number() const { return is_int() || is_double(); }
  bool is_string() const { return type_ == JsonType::kString; }
  bool is_array() const { return type_ == JsonType::kArray; }
  bool is_object() const { return type_ == JsonType::kObject; }

  bool AsBool() const { return bool_; }
  int64_t AsInt() const { return is_double() ? static_cast<int64_t>(double_) : int_; }
  double AsDouble() const { return is_int() ? static_cast<double>(int_) : double_; }
  const std::string& AsString() const { return str_; }
  const Array& AsArray() const { return arr_; }
  Array& AsArray() { return arr_; }
  const Object& AsObject() const { return obj_; }
  Object& AsObject() { return obj_; }

  // Object helpers (linear scan -- objects in practice are small; this is a
  // document store record, not a hash table workload).
  bool Has(const std::string& key) const { return Find(key) != nullptr; }
  const JsonValue* Find(const std::string& key) const {
    for (auto& kv : obj_) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
  void Set(const std::string& key, JsonValue value) {
    for (auto& kv : obj_) {
      if (kv.first == key) {
        kv.second = std::move(value);
        return;
      }
    }
    obj_.emplace_back(key, std::move(value));
  }
  // Convenience accessor with default; does not throw.
  const JsonValue& Get(const std::string& key) const {
    static const JsonValue kNull;
    const JsonValue* v = Find(key);
    return v ? *v : kNull;
  }

  // Serialize to compact JSON text.
  std::string Dump() const;
  // Serialize with object keys sorted recursively -- used wherever bytes
  // must hash/sign identically regardless of field insertion order.
  std::string CanonicalDump() const;

  // Parse JSON text into a JsonValue. Throws std::runtime_error on
  // malformed input (the API layer catches this and returns HTTP 400).
  static JsonValue Parse(const std::string& text);

 private:
  void DumpTo(std::string* out, bool canonical) const;

  JsonType type_;
  bool bool_ = false;
  int64_t int_ = 0;
  double double_ = 0.0;
  std::string str_;
  Array arr_;
  Object obj_;
};

}  // namespace desentry
