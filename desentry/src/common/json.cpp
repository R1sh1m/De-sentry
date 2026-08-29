#include "desentry/common/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace desentry {

namespace {

void EscapeStringTo(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

// ---------------------------------------------------------------------------
// Parser: a small hand-written recursive-descent JSON parser.
// ---------------------------------------------------------------------------
class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text), i_(0), n_(text.size()) {}

  JsonValue ParseValue() {
    SkipWs();
    if (i_ >= n_) Fail("unexpected end of input");
    char c = s_[i_];
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') return JsonValue(ParseString());
    if (c == 't' || c == 'f') return ParseBool();
    if (c == 'n') return ParseNull();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
    Fail(std::string("unexpected character '") + c + "'");
    return JsonValue();  // unreachable
  }

  void ExpectEnd() {
    SkipWs();
    if (i_ != n_) Fail("trailing characters after JSON value");
  }

 private:
  const std::string& s_;
  size_t i_;
  size_t n_;

  [[noreturn]] void Fail(const std::string& msg) {
    throw std::runtime_error("JSON parse error at offset " + std::to_string(i_) + ": " + msg);
  }

  void SkipWs() {
    while (i_ < n_ && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }

  char Peek() { return i_ < n_ ? s_[i_] : '\0'; }

  void Expect(char c) {
    if (i_ >= n_ || s_[i_] != c) Fail(std::string("expected '") + c + "'");
    ++i_;
  }

  JsonValue ParseObject() {
    Expect('{');
    JsonValue::Object obj;
    SkipWs();
    if (Peek() == '}') { ++i_; return JsonValue(std::move(obj)); }
    while (true) {
      SkipWs();
      if (Peek() != '"') Fail("expected string key");
      std::string key = ParseString();
      SkipWs();
      Expect(':');
      JsonValue val = ParseValue();
      obj.emplace_back(std::move(key), std::move(val));
      SkipWs();
      if (Peek() == ',') { ++i_; continue; }
      if (Peek() == '}') { ++i_; break; }
      Fail("expected ',' or '}'");
    }
    return JsonValue(std::move(obj));
  }

  JsonValue ParseArray() {
    Expect('[');
    JsonValue::Array arr;
    SkipWs();
    if (Peek() == ']') { ++i_; return JsonValue(std::move(arr)); }
    while (true) {
      arr.push_back(ParseValue());
      SkipWs();
      if (Peek() == ',') { ++i_; continue; }
      if (Peek() == ']') { ++i_; break; }
      Fail("expected ',' or ']'");
    }
    return JsonValue(std::move(arr));
  }

  std::string ParseString() {
    Expect('"');
    std::string out;
    while (true) {
      if (i_ >= n_) Fail("unterminated string");
      char c = s_[i_++];
      if (c == '"') break;
      if (c == '\\') {
        if (i_ >= n_) Fail("unterminated escape");
        char e = s_[i_++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'u': {
            if (i_ + 4 > n_) Fail("bad \\u escape");
            unsigned int cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s_[i_++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
              else Fail("bad hex digit in \\u escape");
            }
            // Minimal UTF-8 encode (BMP only -- sufficient for the MVP;
            // surrogate pairs are not decoded).
            if (cp < 0x80) {
              out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: Fail("bad escape character");
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  JsonValue ParseBool() {
    if (s_.compare(i_, 4, "true") == 0) { i_ += 4; return JsonValue(true); }
    if (s_.compare(i_, 5, "false") == 0) { i_ += 5; return JsonValue(false); }
    Fail("invalid literal");
    return JsonValue();
  }

  JsonValue ParseNull() {
    if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return JsonValue(nullptr); }
    Fail("invalid literal");
    return JsonValue();
  }

  JsonValue ParseNumber() {
    size_t start = i_;
    bool is_double = false;
    if (Peek() == '-') ++i_;
    while (i_ < n_ && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    if (i_ < n_ && s_[i_] == '.') {
      is_double = true;
      ++i_;
      while (i_ < n_ && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
      is_double = true;
      ++i_;
      if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      while (i_ < n_ && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    std::string tok = s_.substr(start, i_ - start);
    if (tok.empty() || tok == "-") Fail("invalid number");
    if (is_double) return JsonValue(std::stod(tok));
    try {
      return JsonValue(static_cast<int64_t>(std::stoll(tok)));
    } catch (const std::out_of_range&) {
      return JsonValue(std::stod(tok));
    }
  }
};

}  // namespace

JsonValue JsonValue::Parse(const std::string& text) {
  Parser p(text);
  JsonValue v = p.ParseValue();
  p.ExpectEnd();
  return v;
}

void JsonValue::DumpTo(std::string* out, bool canonical) const {
  switch (type_) {
    case JsonType::kNull: *out += "null"; return;
    case JsonType::kBool: *out += bool_ ? "true" : "false"; return;
    case JsonType::kInt: *out += std::to_string(int_); return;
    case JsonType::kDouble: {
      std::ostringstream oss;
      if (std::isfinite(double_)) {
        oss << double_;
      } else {
        oss << 0;  // NaN/Inf are not valid JSON; clamp defensively.
      }
      *out += oss.str();
      return;
    }
    case JsonType::kString: EscapeStringTo(out, str_); return;
    case JsonType::kArray: {
      out->push_back('[');
      for (size_t k = 0; k < arr_.size(); ++k) {
        if (k) out->push_back(',');
        arr_[k].DumpTo(out, canonical);
      }
      out->push_back(']');
      return;
    }
    case JsonType::kObject: {
      out->push_back('{');
      if (!canonical) {
        for (size_t k = 0; k < obj_.size(); ++k) {
          if (k) out->push_back(',');
          EscapeStringTo(out, obj_[k].first);
          out->push_back(':');
          obj_[k].second.DumpTo(out, canonical);
        }
      } else {
        std::vector<const std::pair<std::string, JsonValue>*> sorted;
        sorted.reserve(obj_.size());
        for (auto& kv : obj_) sorted.push_back(&kv);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto* a, auto* b) { return a->first < b->first; });
        for (size_t k = 0; k < sorted.size(); ++k) {
          if (k) out->push_back(',');
          EscapeStringTo(out, sorted[k]->first);
          out->push_back(':');
          sorted[k]->second.DumpTo(out, canonical);
        }
      }
      out->push_back('}');
      return;
    }
  }
}

std::string JsonValue::Dump() const {
  std::string out;
  DumpTo(&out, /*canonical=*/false);
  return out;
}

std::string JsonValue::CanonicalDump() const {
  std::string out;
  DumpTo(&out, /*canonical=*/true);
  return out;
}

}  // namespace desentry
