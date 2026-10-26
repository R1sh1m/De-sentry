#pragma once
// A simple in-memory search index for file/document lookup.
//
// This is a v1 implementation: token-based inverted index with basic
// relevance scoring. It is not persistent (rebuilt on node start) and
// does not support phrase matching or field-weighted scoring beyond
// filename vs content.
//
// Future iterations can add:
// - Persistent index (write to segment store)
// - Phrase matching with position data
// - Field-weighted scoring (filename > content)
// - Stemming / stopword filtering
// - Distributed index (per-node index, merged at query time)

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "desentry/common/status.h"

namespace desentry {

class FileSearchIndex {
 public:
  FileSearchIndex() = default;
  ~FileSearchIndex() = default;

  // Add a file/document to the index
  void IndexFile(const std::string& collection,
                 const std::string& key,
                 const std::string& filename,
                 const std::string& mime,
                 const std::string& text_content,
                 uint64_t size_bytes,
                 int64_t created_ms);

  // Remove a file from the index
  void RemoveFile(const std::string& collection, const std::string& key);

  // Clear the entire index
  void Clear();

  // Search query parameters
  struct SearchQuery {
    std::string query;
    std::string collection_filter;  // empty = all collections
    std::string mime_filter;        // empty = all mime types
    size_t limit = 100;
    size_t offset = 0;
  };

  // Search result
  struct SearchResult {
    std::string collection;
    std::string key;
    std::string filename;
    std::string mime;
    uint64_t size_bytes;
    int64_t created_ms;
    float score;       // Relevance score (higher = more relevant)
    bool filename_match;
    bool content_match;
  };

  // Search the index
  StatusOr<std::vector<SearchResult>> Search(const SearchQuery& query);

  // Get statistics
  size_t NumFiles() const;

 private:
  // Tokenize text: lowercase, split on whitespace/punctuation
  std::vector<std::string> Tokenize(const std::string& text);

  // Index entry: which document and field a term appears in
  struct IndexEntry {
    std::string collection;
    std::string key;
    bool in_filename;  // true if term is in filename, false if in content
  };

  // Inverted index: normalized term -> list of entries
  std::unordered_map<std::string, std::vector<IndexEntry>> index_;

  // Metadata: (collection, key) -> file info
  struct FileMetadata {
    std::string filename;
    std::string mime;
    uint64_t size_bytes;
    int64_t created_ms;
    std::string text_content;  // Store for snippet extraction (optional)
  };
  std::map<std::pair<std::string, std::string>, FileMetadata> metadata_;

  mutable std::mutex mu_;
};

}  // namespace desentry
