#include "desentry/storage/disk_manager.h"

#include <sstream>

#include "desentry/common/json.h"
#include "desentry/common/logger.h"
#include "desentry/common/platform.h"
#include "desentry/security/at_rest.h"
#include "desentry/security/crypto.h"

namespace desentry {

namespace {

std::string ToHex(const std::string& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0xF]);
  }
  return out;
}

}  // namespace

StatusOr<std::unique_ptr<DiskManager>> DiskManager::Open(const std::string& db_file,
                                                         const std::string& dek) {
  // Open for read/write, creating the file if it doesn't exist. fstream
  // can't create-if-missing directly, so probe first.
  std::fstream probe(db_file, std::ios::in | std::ios::binary);
  const bool existed = probe.is_open();
  int64_t existing_size = 0;
  if (existed) {
    probe.close();
    existing_size = FileSize(db_file);
  } else {
    probe.close();
    std::ofstream create(db_file, std::ios::out | std::ios::binary);
    if (!create.is_open()) {
      return Status::IOError("cannot create data file: " + db_file);
    }
    create.close();
  }

  std::fstream file(db_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!file.is_open()) {
    return Status::IOError("cannot open data file: " + db_file);
  }

  const bool encrypted = !dek.empty();
  std::string file_tag;
  std::string subkey;
  if (encrypted) {
    if (dek.size() != 32) {
      return Status::InvalidArgument("at-rest: DEK must be 32 bytes");
    }
    auto tag_or = at_rest::ReadPageHeaderFile(db_file);
    if (!tag_or.ok()) {
      if (tag_or.status().code() != StatusCode::kNotFound) return tag_or.status();
      if (existed && existing_size > 0) {
        // Fail closed: sealing over a plaintext file in place would mix
        // strides mid-file. The offline --re-encrypt tool does this rewrite.
        return Status::Corruption("at-rest: plaintext data file " + db_file +
                                  " opened with encryption enabled; migrate it with "
                                  "`desentryd --re-encrypt`");
      }
      // New file: mint its random tag and write the sidecar header.
      std::string raw_tag;
      try {
        raw_tag = crypto::RandomBytes(16);
      } catch (const std::exception& e) {
        return Status::Internal(std::string("at-rest: RNG failure: ") + e.what());
      }
      file_tag = ToHex(raw_tag);
      at_rest::Zeroize(raw_tag);
      Status st = at_rest::WritePageHeaderFile(db_file, file_tag);
      if (!st.ok()) return st;
    } else {
      file_tag = tag_or.value();
    }
    auto subkey_or = at_rest::FileSubkey(dek, file_tag);
    if (!subkey_or.ok()) return subkey_or.status();
    subkey = subkey_or.value();
  } else {
    auto tag_or = at_rest::ReadPageHeaderFile(db_file);
    if (tag_or.ok()) {
      // Fail closed the other way: an encrypted file opened without a key
      // must never be misread as plaintext pages.
      return Status::Corruption("at-rest: encrypted data file " + db_file +
                                " opened without encryption enabled (missing unlock key?)");
    }
    if (tag_or.status().code() != StatusCode::kNotFound) return tag_or.status();
  }

  // Size via the platform shim (platform.h is the only file that touches
  // stat/chmod APIs, so this stays portable to Windows).
  const size_t stride = encrypted ? at_rest::kSealedPageSize : kPageSize;
  const int64_t file_size = FileSize(db_file);
  if (file_size % static_cast<int64_t>(stride) != 0) {
    // A torn final page (crash mid-append). Reads of it zero-fill exactly
    // like the plaintext path; the id is reused by the next allocation and
    // WAL replay heals the content. Loud, not fatal.
    DSN_LOG_WARN("disk", "file " << db_file << " size " << file_size << " is not a multiple of "
                                 << stride << "; torn tail page will read as zeroed");
  }
  const int64_t next_page_id = file_size / static_cast<int64_t>(stride);

  std::unique_ptr<DiskManager> mgr(
      new DiskManager(std::move(file), db_file, next_page_id, std::move(subkey), std::move(file_tag),
                      encrypted, stride));
  DSN_LOG_INFO("disk", "opened " << db_file << " with " << next_page_id << " existing pages"
                                 << (encrypted ? " [sealed]" : ""));
  return mgr;
}

DiskManager::~DiskManager() {
  at_rest::Zeroize(subkey_);
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

Status DiskManager::ReadPage(page_id_t page_id, char* out) {
  std::lock_guard<std::mutex> lock(io_mu_);
  auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(stride_);
  file_.clear();
  file_.seekg(offset);
  if (!file_.good()) {
    std::memset(out, 0, kPageSize);
    return Status::OK();
  }
  if (!encrypted_) {
    file_.read(out, static_cast<std::streamsize>(kPageSize));
    auto read_bytes = file_.gcount();
    if (read_bytes < static_cast<std::streamsize>(kPageSize)) {
      // Short read (page never fully written yet) -- zero-fill the remainder.
      std::memset(out + read_bytes, 0, kPageSize - static_cast<size_t>(read_bytes));
    }
    file_.clear();
    return Status::OK();
  }
  std::string sealed(at_rest::kSealedPageSize, '\0');
  file_.read(sealed.data(), static_cast<std::streamsize>(at_rest::kSealedPageSize));
  file_.clear();
  if (file_.gcount() < static_cast<std::streamsize>(at_rest::kSealedPageSize)) {
    // Torn final page: zero-fill like the plaintext path; WAL replay heals.
    std::memset(out, 0, kPageSize);
    return Status::OK();
  }
  return at_rest::OpenPage(subkey_, page_id, file_tag_, sealed.data(), out);
}

Status DiskManager::WritePage(page_id_t page_id, const char* data) {
  std::lock_guard<std::mutex> lock(io_mu_);
  if (!encrypted_) {
    auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(kPageSize);
    file_.clear();
    file_.seekp(offset);
    file_.write(data, static_cast<std::streamsize>(kPageSize));
    if (!file_.good()) {
      return Status::IOError("write failed for page " + std::to_string(page_id));
    }
    file_.flush();
    return Status::OK();
  }
  std::string sealed(at_rest::kSealedPageSize, '\0');
  Status st = at_rest::SealPage(subkey_, page_id, file_tag_, data, sealed.data());
  if (!st.ok()) return st;
  auto offset =
      static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(at_rest::kSealedPageSize);
  file_.clear();
  file_.seekp(offset);
  file_.write(sealed.data(), static_cast<std::streamsize>(sealed.size()));
  if (!file_.good()) {
    return Status::IOError("write failed for page " + std::to_string(page_id));
  }
  file_.flush();
  return Status::OK();
}

page_id_t DiskManager::AllocatePage() {
  return static_cast<page_id_t>(next_page_id_.fetch_add(1));
}

Status DiskManager::Sync() {
  std::lock_guard<std::mutex> lock(io_mu_);
  file_.flush();
  std::string sync_err;
  if (!SyncFileByPath(path_, &sync_err)) {
    return Status::IOError("disk sync failed: " + sync_err);
  }
  return Status::OK();
}

}  // namespace desentry
