#pragma once
// Thin bridge between the storage layer and the CRDT document type. The
// actual binary codec lives as CrdtValue::Encode()/Decode() (crdt/document.h)
// -- keeping the encode/decode logic as methods on the type itself avoids a
// pile of friend declarations, while this header is what storage/*.cpp
// includes, so the storage layer's dependency on "documents are encoded
// this way" is explicit and file-structure matches ARCHITECTURE.md.

#include "desentry/crdt/document.h"

namespace desentry {

inline std::string EncodeDocument(const CrdtValue& doc) { return doc.Encode(); }
inline CrdtValue DecodeDocument(const std::string& bytes) { return CrdtValue::Decode(bytes); }

}  // namespace desentry
