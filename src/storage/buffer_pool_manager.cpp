#include "desentry/storage/buffer_pool_manager.h"

#include "desentry/common/logger.h"

namespace desentry {

// ---------------------------------------------------------------------------
// LRUReplacer
// ---------------------------------------------------------------------------

void LRUReplacer::RecordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = position_.find(frame_id);
  if (it != position_.end()) {
    lru_list_.erase(it->second);
  }
  lru_list_.push_front(frame_id);
  position_[frame_id] = lru_list_.begin();
}

void LRUReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = position_.find(frame_id);
  if (it != position_.end()) {
    lru_list_.erase(it->second);
    position_.erase(it);
  }
}

bool LRUReplacer::Victim(frame_id_t* out_frame_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (lru_list_.empty()) return false;
  *out_frame_id = lru_list_.back();
  position_.erase(lru_list_.back());
  lru_list_.pop_back();
  return true;
}

size_t LRUReplacer::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lru_list_.size();
}

// ---------------------------------------------------------------------------
// BufferPoolManager
// ---------------------------------------------------------------------------

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager)
    : pages_(pool_size), disk_manager_(disk_manager) {
  free_list_.reserve(pool_size);
  for (size_t i = 0; i < pool_size; ++i) {
    free_list_.push_back(static_cast<frame_id_t>(i));
  }
}

Page* BufferPoolManager::FindFreeOrVictimFrame(frame_id_t* out_frame) {
  if (!free_list_.empty()) {
    *out_frame = free_list_.back();
    free_list_.pop_back();
    return &pages_[static_cast<size_t>(*out_frame)];
  }
  frame_id_t victim;
  if (!replacer_.Victim(&victim)) {
    return nullptr;  // pool exhausted: every frame is pinned
  }
  Page& victim_page = pages_[static_cast<size_t>(victim)];
  if (victim_page.IsDirty()) {
    disk_manager_->WritePage(victim_page.GetPageId(), victim_page.GetData());
  }
  page_table_.erase(victim_page.GetPageId());
  *out_frame = victim;
  return &victim_page;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    Page& page = pages_[static_cast<size_t>(it->second)];
    if (page.pin_count_ == 0) replacer_.Pin(it->second);
    page.pin_count_++;
    return &page;
  }

  frame_id_t frame_id;
  Page* frame = FindFreeOrVictimFrame(&frame_id);
  if (frame == nullptr) return nullptr;

  frame->ResetMemory();
  disk_manager_->ReadPage(page_id, frame->GetData());
  frame->page_id_ = page_id;
  frame->pin_count_ = 1;
  frame->is_dirty_ = false;
  page_table_[page_id] = frame_id;
  return frame;
}

Page* BufferPoolManager::NewPage(page_id_t* out_page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  frame_id_t frame_id;
  Page* frame = FindFreeOrVictimFrame(&frame_id);
  if (frame == nullptr) return nullptr;

  page_id_t new_id = disk_manager_->AllocatePage();
  frame->ResetMemory();
  frame->page_id_ = new_id;
  frame->pin_count_ = 1;
  frame->is_dirty_ = true;  // brand new page: not yet on disk
  page_table_[new_id] = frame_id;
  *out_page_id = new_id;
  return frame;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  Page& page = pages_[static_cast<size_t>(it->second)];
  if (page.pin_count_ <= 0) return false;
  if (is_dirty) page.is_dirty_ = true;
  page.pin_count_--;
  if (page.pin_count_ == 0) replacer_.RecordAccess(it->second);
  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;
  Page& page = pages_[static_cast<size_t>(it->second)];
  disk_manager_->WritePage(page_id, page.GetData());
  page.is_dirty_ = false;
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> lock(latch_);
  for (auto& [page_id, frame_id] : page_table_) {
    Page& page = pages_[static_cast<size_t>(frame_id)];
    if (page.IsDirty()) {
      disk_manager_->WritePage(page_id, page.GetData());
      page.is_dirty_ = false;
    }
  }
  disk_manager_->Sync();
}

}  // namespace desentry
