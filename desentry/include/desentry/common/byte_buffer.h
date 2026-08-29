#pragma once
// Small binary encode/decode helpers shared by the document codec
// (storage/document_codec.h) and the P2P wire protocol (net/wire_protocol.h)
// -- both need "append a length-prefixed blob / fixed-width integer" and
// nothing fancier, so one tiny utility replaces two copies of the same code.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace desentry {

class ByteWriter {
 public:
  void U8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void U16(uint16_t v) { Append(&v, 2); }
  void U32(uint32_t v) { Append(&v, 4); }
  void U64(uint64_t v) { Append(&v, 8); }
  void I64(int64_t v) { Append(&v, 8); }
  void F64(double v) { Append(&v, 8); }
  void Bytes(const std::string& s) { U32(static_cast<uint32_t>(s.size())); buf_ += s; }
  void RawBytes(const std::string& s) { buf_ += s; }  // no length prefix -- caller manages framing

  const std::string& str() const { return buf_; }
  std::string TakeString() { return std::move(buf_); }

 private:
  void Append(const void* p, size_t n) {
    size_t off = buf_.size();
    buf_.resize(off + n);
    std::memcpy(buf_.data() + off, p, n);
  }
  std::string buf_;
};

class ByteReader {
 public:
  ByteReader(const char* data, size_t len) : data_(data), len_(len), pos_(0) {}
  explicit ByteReader(const std::string& s) : data_(s.data()), len_(s.size()), pos_(0) {}

  uint8_t U8() { Need(1); return static_cast<uint8_t>(data_[pos_++]); }
  uint16_t U16() { uint16_t v; Read(&v, 2); return v; }
  uint32_t U32() { uint32_t v; Read(&v, 4); return v; }
  uint64_t U64() { uint64_t v; Read(&v, 8); return v; }
  int64_t I64() { int64_t v; Read(&v, 8); return v; }
  double F64() { double v; Read(&v, 8); return v; }
  std::string Bytes() {
    uint32_t n = U32();
    Need(n);
    std::string s(data_ + pos_, n);
    pos_ += n;
    return s;
  }
  std::string RawBytes(size_t n) {
    Need(n);
    std::string s(data_ + pos_, n);
    pos_ += n;
    return s;
  }

  size_t remaining() const { return len_ - pos_; }
  size_t position() const { return pos_; }

 private:
  void Need(size_t n) {
    if (pos_ + n > len_) throw std::runtime_error("ByteReader: unexpected end of buffer");
  }
  void Read(void* out, size_t n) {
    Need(n);
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
  }

  const char* data_;
  size_t len_;
  size_t pos_;
};

}  // namespace desentry
