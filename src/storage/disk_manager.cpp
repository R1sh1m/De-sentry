#include "desentry/storage/disk_manager.h"

#include <sys/stat.h>

#include "desentry/common/logger.h"

namespace desentry {

StatusOr<std::unique_ptr<DiskManager>> DiskManager::Open(const std::string& db_file) {
  // Open for read/write, creating the file if it doesn't exist. fstream
  // can't create-if-missing directly, so probe first.
  std::fstream probe(db_file, std::ios::in | std::ios::binary);
  if (!probe.is_open()) {
    std::ofstream create(db_file, std::ios::out | std::ios::binary);
    if (!create.is_open()) {
      return Status::IOError("cannot create data file: " + db_file);
    }
    create.close();
  } else {
    probe.close();
  }

  std::fstream file(db_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!file.is_open()) {
    return Status::IOError("cannot open data file: " + db_file);
  }

  struct stat st{};
  int64_t next_page_id = 0;
  if (::stat(db_file.c_str(), &st) == 0) {
    next_page_id = static_cast<int64_t>(st.st_size) / static_cast<int64_t>(kPageSize);
  }

  std::unique_ptr<DiskManager> mgr(new DiskManager(std::move(file), db_file, next_page_id));
  DSN_LOG_INFO("disk", "opened " << db_file << " with " << next_page_id << " existing pages");
  return mgr;
}

DiskManager::~DiskManager() {
  if (file_.is_open()) {
    file_.flush();
    file_.close();
  }
}

Status DiskManager::ReadPage(page_id_t page_id, char* out) {
  std::lock_guard<std::mutex> lock(io_mu_);
  auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(kPageSize);
  file_.clear();
  file_.seekg(offset);
  if (!file_.good()) {
    std::memset(out, 0, kPageSize);
    return Status::OK();
  }
  file_.read(out, static_cast<std::streamsize>(kPageSize));
  auto read_bytes = file_.gcount();
  if (read_bytes < static_cast<std::streamsize>(kPageSize)) {
    // Short read (page never fully written yet) -- zero-fill the remainder.
    std::memset(out + read_bytes, 0, kPageSize - static_cast<size_t>(read_bytes));
  }
  file_.clear();
  return Status::OK();
}

Status DiskManager::WritePage(page_id_t page_id, const char* data) {
  std::lock_guard<std::mutex> lock(io_mu_);
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

page_id_t DiskManager::AllocatePage() {
  return static_cast<page_id_t>(next_page_id_.fetch_add(1));
}

Status DiskManager::Sync() {
  std::lock_guard<std::mutex> lock(io_mu_);
  file_.flush();
  return Status::OK();
}

}  // namespace desentry
