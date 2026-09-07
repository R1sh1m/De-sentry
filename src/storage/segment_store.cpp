#include "desentry/storage/segment_store.h"

#include <cstring>

#include "desentry/common/crc32.h"
#include "desentry/common/logger.h"

namespace desentry {

namespace {

struct SegPageHeader {
  uint32_t magic;
  uint32_t chunk_len;
  int32_t next_page_id;
  uint32_t crc;
};

void WriteHeader(char* data, const SegPageHeader& h) {
  std::memcpy(data + 0, &h.magic, 4);
  std::memcpy(data + 4, &h.chunk_len, 4);
  std::memcpy(data + 8, &h.next_page_id, 4);
  std::memcpy(data + 12, &h.crc, 4);
}

SegPageHeader ReadHeader(const char* data) {
  SegPageHeader h;
  std::memcpy(&h.magic, data + 0, 4);
  std::memcpy(&h.chunk_len, data + 4, 4);
  std::memcpy(&h.next_page_id, data + 8, 4);
  std::memcpy(&h.crc, data + 12, 4);
  return h;
}

}  // namespace

StatusOr<std::unique_ptr<SegmentStore>> SegmentStore::Open(const std::string& file_path,
                                                             size_t buffer_pool_pages) {
  std::unique_ptr<SegmentStore> store(new SegmentStore());
  auto disk_or = DiskManager::Open(file_path);
  if (!disk_or.ok()) return disk_or.status();
  store->disk_ = std::move(disk_or.value());
  store->pool_ = std::make_unique<BufferPoolManager>(buffer_pool_pages, store->disk_.get());
  return store;
}

SegmentStore::~SegmentStore() {
  if (pool_) pool_->FlushAllPages();
}

StatusOr<page_id_t> SegmentStore::AppendBlob(const std::string& bytes) {
  std::lock_guard<std::mutex> lock(mu_);

  // Chunk the payload, then link the pages back-to-front: a page's header
  // has to name its successor, and the successor's id is only known after
  // it is allocated. Building the chain in reverse means every header is
  // written exactly once, with no fix-up pass that a crash could interrupt
  // halfway.
  const size_t total = bytes.size();
  size_t num_chunks = total == 0 ? 1 : (total + kSegmentPagePayload - 1) / kSegmentPagePayload;

  page_id_t next = kInvalidPageId;
  page_id_t first = kInvalidPageId;
  for (size_t i = num_chunks; i-- > 0;) {
    const size_t offset = i * kSegmentPagePayload;
    const size_t len = total <= offset ? 0 : std::min(kSegmentPagePayload, total - offset);

    page_id_t page_id = kInvalidPageId;
    Page* page = pool_->NewPage(&page_id);
    if (page == nullptr) {
      return Status::OutOfSpace("segment store: buffer pool exhausted allocating a segment page");
    }
    char* data = page->GetData();
    std::memset(data, 0, kPageSize);

    SegPageHeader h;
    h.magic = kSegmentPageMagic;
    h.chunk_len = static_cast<uint32_t>(len);
    h.next_page_id = next;
    h.crc = len == 0 ? 0u : Crc32(bytes.data() + offset, len);
    WriteHeader(data, h);
    if (len > 0) std::memcpy(data + kSegmentPageHeaderBytes, bytes.data() + offset, len);

    pool_->UnpinPage(page_id, true);
    next = page_id;
    first = page_id;
  }
  return first;
}

StatusOr<std::string> SegmentStore::ReadBlob(page_id_t first_page_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string out;
  page_id_t cursor = first_page_id;
  // A chain longer than the file has pages means the next-pointers form a
  // cycle (only reachable from a corrupted file); bound the walk instead of
  // hanging.
  const int64_t max_pages = disk_->NumAllocatedPages() + 1;
  for (int64_t visited = 0; cursor != kInvalidPageId; ++visited) {
    if (visited > max_pages) return Status::Corruption("segment store: page chain does not terminate");
    Page* page = pool_->FetchPage(cursor);
    if (page == nullptr) return Status::Internal("segment store: cannot fetch page " + std::to_string(cursor));
    const char* data = page->GetData();
    SegPageHeader h = ReadHeader(data);
    if (h.magic != kSegmentPageMagic || h.chunk_len > kSegmentPagePayload) {
      pool_->UnpinPage(cursor, false);
      return Status::Corruption("segment store: bad page header at page " + std::to_string(cursor));
    }
    if (h.chunk_len > 0 && Crc32(data + kSegmentPageHeaderBytes, h.chunk_len) != h.crc) {
      pool_->UnpinPage(cursor, false);
      return Status::Corruption("segment store: CRC mismatch at page " + std::to_string(cursor));
    }
    out.append(data + kSegmentPageHeaderBytes, h.chunk_len);
    page_id_t next = h.next_page_id;
    pool_->UnpinPage(cursor, false);
    cursor = next;
  }
  return out;
}

Status SegmentStore::VerifyBlob(page_id_t first_page_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  page_id_t cursor = first_page_id;
  const int64_t max_pages = disk_->NumAllocatedPages() + 1;
  for (int64_t visited = 0; cursor != kInvalidPageId; ++visited) {
    if (visited > max_pages) return Status::Corruption("segment store: page chain does not terminate");
    Page* page = pool_->FetchPage(cursor);
    if (page == nullptr) return Status::Internal("segment store: cannot fetch page " + std::to_string(cursor));
    const char* data = page->GetData();
    SegPageHeader h = ReadHeader(data);
    bool bad = h.magic != kSegmentPageMagic || h.chunk_len > kSegmentPagePayload ||
               (h.chunk_len > 0 && Crc32(data + kSegmentPageHeaderBytes, h.chunk_len) != h.crc);
    page_id_t next = h.next_page_id;
    pool_->UnpinPage(cursor, false);
    if (bad) return Status::Corruption("segment store: page " + std::to_string(cursor) + " failed verification");
    cursor = next;
  }
  return Status::OK();
}

uint64_t SegmentStore::BytesOnDisk() const {
  int64_t pages = disk_ ? disk_->NumAllocatedPages() : 0;
  return static_cast<uint64_t>(pages < 0 ? 0 : pages) * kPageSize;
}

Status SegmentStore::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  pool_->FlushAllPages();
  return disk_->Sync();
}

}  // namespace desentry
