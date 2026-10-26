#include "desentry/search/file_index.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace desentry {

namespace {

// Normalize a token: lowercase, strip punctuation
std::string NormalizeToken(const std::string& token) {
  std::string result;
  result.reserve(token.size());
  for (char c : token) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return result;
}

}  // namespace

void FileSearchIndex::IndexFile(const std::string& collection,
                                 const std::string& key,
                                 const std::string& filename,
                                 const std::string& mime,
                                 const std::string& text_content,
                                 uint64_t size_bytes,
                                 int64_t created_ms) {
  std::lock_guard<std::mutex> lock(mu_);

  // Remove any existing entry for this file
  auto meta_key = std::make_pair(collection, key);
  if (metadata_.count(meta_key)) {
    // Remove old index entries
    const auto& old_meta = metadata_[meta_key];
    auto old_tokens = Tokenize(old_meta.filename);
    for (const auto& tok : old_tokens) {
      std::string norm = NormalizeToken(tok);
      if (norm.empty()) continue;
      auto& entries = index_[norm];
      entries.erase(
          std::remove_if(entries.begin(), entries.end(),
                         [&](const IndexEntry& e) {
                           return e.collection == collection && e.key == key && e.in_filename;
                         }),
          entries.end());
    }
    old_tokens = Tokenize(text_content);
    for (const auto& tok : old_tokens) {
      std::string norm = NormalizeToken(tok);
      if (norm.empty()) continue;
      auto& entries = index_[norm];
      entries.erase(
          std::remove_if(entries.begin(), entries.end(),
                         [&](const IndexEntry& e) {
                           return e.collection == collection && e.key == key && !e.in_filename;
                         }),
          entries.end());
    }
  }

  // Store metadata
  FileMetadata meta;
  meta.filename = filename;
  meta.mime = mime;
  meta.size_bytes = size_bytes;
  meta.created_ms = created_ms;
  meta.text_content = text_content;
  metadata_[meta_key] = meta;

  // Index filename tokens
  auto filename_tokens = Tokenize(filename);
  for (const auto& tok : filename_tokens) {
    std::string norm = NormalizeToken(tok);
    if (norm.empty()) continue;
    IndexEntry entry;
    entry.collection = collection;
    entry.key = key;
    entry.in_filename = true;
    index_[norm].push_back(entry);
  }

  // Index content tokens
  auto content_tokens = Tokenize(text_content);
  for (const auto& tok : content_tokens) {
    std::string norm = NormalizeToken(tok);
    if (norm.empty()) continue;
    IndexEntry entry;
    entry.collection = collection;
    entry.key = key;
    entry.in_filename = false;
    index_[norm].push_back(entry);
  }
}

void FileSearchIndex::RemoveFile(const std::string& collection, const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);

  auto meta_key = std::make_pair(collection, key);
  auto it = metadata_.find(meta_key);
  if (it == metadata_.end()) return;

  const auto& meta = it->second;

  // Remove filename tokens
  auto tokens = Tokenize(meta.filename);
  for (const auto& tok : tokens) {
    std::string norm = NormalizeToken(tok);
    if (norm.empty()) continue;
    auto idx_it = index_.find(norm);
    if (idx_it != index_.end()) {
      auto& entries = idx_it->second;
      entries.erase(
          std::remove_if(entries.begin(), entries.end(),
                         [&](const IndexEntry& e) {
                           return e.collection == collection && e.key == key && e.in_filename;
                         }),
          entries.end());
      if (entries.empty()) {
        index_.erase(idx_it);
      }
    }
  }

  // Remove content tokens
  tokens = Tokenize(meta.text_content);
  for (const auto& tok : tokens) {
    std::string norm = NormalizeToken(tok);
    if (norm.empty()) continue;
    auto idx_it = index_.find(norm);
    if (idx_it != index_.end()) {
      auto& entries = idx_it->second;
      entries.erase(
          std::remove_if(entries.begin(), entries.end(),
                         [&](const IndexEntry& e) {
                           return e.collection == collection && e.key == key && !e.in_filename;
                         }),
          entries.end());
      if (entries.empty()) {
        index_.erase(idx_it);
      }
    }
  }

  metadata_.erase(it);
}

void FileSearchIndex::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  index_.clear();
  metadata_.clear();
}

StatusOr<std::vector<FileSearchIndex::SearchResult>> FileSearchIndex::Search(
    const SearchQuery& query) {
  std::lock_guard<std::mutex> lock(mu_);

  std::vector<SearchResult> results;

  if (query.query.empty()) {
    // Return all files matching filters
    for (const auto& [meta_key, meta] : metadata_) {
      if (!query.collection_filter.empty() && meta_key.first != query.collection_filter) {
        continue;
      }
      if (!query.mime_filter.empty() && meta.mime != query.mime_filter) {
        continue;
      }
      SearchResult r;
      r.collection = meta_key.first;
      r.key = meta_key.second;
      r.filename = meta.filename;
      r.mime = meta.mime;
      r.size_bytes = meta.size_bytes;
      r.created_ms = meta.created_ms;
      r.score = 0.0f;
      r.filename_match = false;
      r.content_match = false;
      results.push_back(r);
    }
  } else {
    // Tokenize query
    auto query_tokens = Tokenize(query.query);
    std::unordered_set<std::string> query_terms;
    for (const auto& tok : query_tokens) {
      std::string norm = NormalizeToken(tok);
      if (!norm.empty()) query_terms.insert(norm);
    }

    if (query_terms.empty()) {
      return results;  // No valid query terms
    }

    // Score each document
    std::unordered_map<std::string, float> doc_scores;  // "collection::key" -> score
    std::unordered_map<std::string, bool> doc_filename_match;
    std::unordered_map<std::string, bool> doc_content_match;

    for (const auto& term : query_terms) {
      auto it = index_.find(term);
      if (it == index_.end()) continue;

      for (const auto& entry : it->second) {
        if (!query.collection_filter.empty() && entry.collection != query.collection_filter) {
          continue;
        }
        std::string doc_key = entry.collection + "::" + entry.key;
        // Filename matches score higher (10x) than content
        float score = entry.in_filename ? 10.0f : 1.0f;
        doc_scores[doc_key] += score;
        if (entry.in_filename) doc_filename_match[doc_key] = true;
        else doc_content_match[doc_key] = true;
      }
    }

    // Build results
    for (const auto& [doc_key, score] : doc_scores) {
      auto sep = doc_key.find("::");
      if (sep == std::string::npos) continue;
      std::string collection = doc_key.substr(0, sep);
      std::string key = doc_key.substr(sep + 2);

      auto meta_it = metadata_.find(std::make_pair(collection, key));
      if (meta_it == metadata_.end()) continue;
      const auto& meta = meta_it->second;

      if (!query.mime_filter.empty() && meta.mime != query.mime_filter) {
        continue;
      }

      SearchResult r;
      r.collection = collection;
      r.key = key;
      r.filename = meta.filename;
      r.mime = meta.mime;
      r.size_bytes = meta.size_bytes;
      r.created_ms = meta.created_ms;
      r.score = score;
      r.filename_match = doc_filename_match[doc_key];
      r.content_match = doc_content_match[doc_key];
      results.push_back(r);
    }
  }

  // Sort by score (descending), then by filename
  std::sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.filename < b.filename;
            });

  // Apply offset and limit
  if (query.offset >= results.size()) {
    return std::vector<SearchResult>();
  }
  size_t end = std::min(query.offset + query.limit, results.size());
  return std::vector<SearchResult>(results.begin() + query.offset, results.begin() + end);
}

size_t FileSearchIndex::NumFiles() const {
  std::lock_guard<std::mutex> lock(mu_);
  return metadata_.size();
}

std::vector<std::string> FileSearchIndex::Tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '.' || c == ';' ||
        c == ':' || c == '!' || c == '?' || c == '(' || c == ')' || c == '[' || c == ']' ||
        c == '{' || c == '}' || c == '"' || c == '\'' || c == '-' || c == '_' || c == '/') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

}  // namespace desentry