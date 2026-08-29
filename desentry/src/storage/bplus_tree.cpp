#include "desentry/storage/bplus_tree.h"

#include <algorithm>
#include <cstring>

#include "desentry/common/logger.h"

namespace desentry {

namespace {

#pragma pack(push, 1)
struct BNodeHeader {
  uint8_t is_leaf;
  int16_t num_keys;
  page_id_t parent_page_id;
  page_id_t next_leaf_page_id;  // leaves only; kInvalidPageId for internal nodes / last leaf
};
struct PackedRid {
  page_id_t page_id;
  slot_id_t slot_id;
};
#pragma pack(pop)

BNodeHeader* Hdr(Page* p) { return reinterpret_cast<BNodeHeader*>(p->GetData()); }
const BNodeHeader* Hdr(const Page* p) { return reinterpret_cast<const BNodeHeader*>(p->GetData()); }

bool IsLeaf(const Page* p) { return Hdr(p)->is_leaf != 0; }
int NumKeys(const Page* p) { return Hdr(p)->num_keys; }

constexpr size_t kHeaderSize = sizeof(BNodeHeader);
constexpr size_t kLeafKeysOffset = kHeaderSize;
constexpr size_t kLeafRidsOffset = kLeafKeysOffset + kMaxKeysPerNode * kMaxKeyBytes;
constexpr size_t kInternalChildrenOffset = kHeaderSize;
constexpr size_t kInternalKeysOffset = kInternalChildrenOffset + (kMaxKeysPerNode + 1) * sizeof(page_id_t);

static_assert(kLeafRidsOffset + kMaxKeysPerNode * sizeof(PackedRid) <= kPageSize, "leaf node too large for page");
static_assert(kInternalKeysOffset + kMaxKeysPerNode * kMaxKeyBytes <= kPageSize, "internal node too large for page");

std::string GetKeyAt(const Page* p, int i, bool leaf) {
  const char* base = p->GetData() + (leaf ? kLeafKeysOffset : kInternalKeysOffset) +
                      static_cast<size_t>(i) * kMaxKeyBytes;
  size_t len = strnlen(base, kMaxKeyBytes);
  return std::string(base, len);
}
void SetKeyAt(Page* p, int i, bool leaf, const std::string& key) {
  char* base = p->GetData() + (leaf ? kLeafKeysOffset : kInternalKeysOffset) +
               static_cast<size_t>(i) * kMaxKeyBytes;
  std::memset(base, 0, kMaxKeyBytes);
  std::memcpy(base, key.data(), std::min(key.size(), kMaxKeyBytes));
}
RID GetRidAt(const Page* p, int i) {
  PackedRid pr;
  std::memcpy(&pr, p->GetData() + kLeafRidsOffset + static_cast<size_t>(i) * sizeof(PackedRid), sizeof(PackedRid));
  return RID{pr.page_id, pr.slot_id};
}
void SetRidAt(Page* p, int i, const RID& rid) {
  PackedRid pr{rid.page_id, rid.slot_id};
  std::memcpy(p->GetData() + kLeafRidsOffset + static_cast<size_t>(i) * sizeof(PackedRid), &pr, sizeof(PackedRid));
}
page_id_t GetChildAt(const Page* p, int i) {
  page_id_t id;
  std::memcpy(&id, p->GetData() + kInternalChildrenOffset + static_cast<size_t>(i) * sizeof(page_id_t), sizeof(page_id_t));
  return id;
}
void SetChildAt(Page* p, int i, page_id_t id) {
  std::memcpy(p->GetData() + kInternalChildrenOffset + static_cast<size_t>(i) * sizeof(page_id_t), &id, sizeof(page_id_t));
}

void InitLeaf(Page* p, page_id_t parent) {
  Hdr(p)->is_leaf = 1;
  Hdr(p)->num_keys = 0;
  Hdr(p)->parent_page_id = parent;
  Hdr(p)->next_leaf_page_id = kInvalidPageId;
}
void InitInternal(Page* p, page_id_t parent) {
  Hdr(p)->is_leaf = 0;
  Hdr(p)->num_keys = 0;
  Hdr(p)->parent_page_id = parent;
  Hdr(p)->next_leaf_page_id = kInvalidPageId;
}

}  // namespace

StatusOr<page_id_t> BPlusTree::CreateNew(BufferPoolManager* bpm) {
  page_id_t root_id;
  Page* root = bpm->NewPage(&root_id);
  if (root == nullptr) return Status::OutOfSpace("buffer pool exhausted creating B+Tree root");
  InitLeaf(root, kInvalidPageId);
  bpm->UnpinPage(root_id, true);
  return root_id;
}

page_id_t BPlusTree::FindLeaf(const std::string& key) const {
  page_id_t cur = root_page_id_;
  while (true) {
    Page* p = bpm_->FetchPage(cur);
    if (p == nullptr) return kInvalidPageId;
    if (IsLeaf(p)) {
      bpm_->UnpinPage(cur, false);
      return cur;
    }
    int n = NumKeys(p);
    int idx = n;
    for (int i = 0; i < n; ++i) {
      if (key < GetKeyAt(p, i, false)) { idx = i; break; }
    }
    page_id_t child = GetChildAt(p, idx);
    bpm_->UnpinPage(cur, false);
    cur = child;
  }
}

bool BPlusTree::Search(const std::string& key, RID* out_rid) const {
  std::lock_guard<std::mutex> lock(tree_mu_);
  page_id_t leaf_id = FindLeaf(key);
  if (leaf_id == kInvalidPageId) return false;
  Page* p = bpm_->FetchPage(leaf_id);
  if (p == nullptr) return false;
  int n = NumKeys(p);
  bool found = false;
  for (int i = 0; i < n; ++i) {
    if (GetKeyAt(p, i, true) == key) {
      *out_rid = GetRidAt(p, i);
      found = true;
      break;
    }
  }
  bpm_->UnpinPage(leaf_id, false);
  return found;
}

Status BPlusTree::Insert(const std::string& key, const RID& rid) {
  if (key.empty() || key.size() >= kMaxKeyBytes) {
    return Status::InvalidArgument("key length must be in (0, " + std::to_string(kMaxKeyBytes) + ") bytes");
  }
  std::lock_guard<std::mutex> lock(tree_mu_);
  page_id_t leaf_id = FindLeaf(key);
  if (leaf_id == kInvalidPageId) return Status::Internal("FindLeaf failed");
  return InsertIntoLeaf(leaf_id, key, rid);
}

Status BPlusTree::InsertIntoLeaf(page_id_t leaf_id, const std::string& key, const RID& rid) {
  Page* p = bpm_->FetchPage(leaf_id);
  if (p == nullptr) return Status::OutOfSpace("buffer pool exhausted");
  int n = NumKeys(p);

  // Locate insertion position / check for an existing key (upsert).
  int idx = n;
  for (int i = 0; i < n; ++i) {
    std::string k = GetKeyAt(p, i, true);
    if (k == key) {
      SetRidAt(p, i, rid);
      bpm_->UnpinPage(leaf_id, true);
      return Status::OK();
    }
    if (key < k) { idx = i; break; }
  }

  if (n < kMaxKeysPerNode) {
    for (int i = n; i > idx; --i) {
      SetKeyAt(p, i, true, GetKeyAt(p, i - 1, true));
      SetRidAt(p, i, GetRidAt(p, i - 1));
    }
    SetKeyAt(p, idx, true, key);
    SetRidAt(p, idx, rid);
    Hdr(p)->num_keys = static_cast<int16_t>(n + 1);
    bpm_->UnpinPage(leaf_id, true);
    return Status::OK();
  }

  // Full leaf: split. Gather all n existing entries + the new one, sorted.
  std::vector<std::pair<std::string, RID>> entries;
  entries.reserve(n + 1);
  for (int i = 0; i < n; ++i) entries.emplace_back(GetKeyAt(p, i, true), GetRidAt(p, i));
  entries.insert(entries.begin() + idx, {key, rid});

  int left_count = static_cast<int>(entries.size()) / 2;
  int right_count = static_cast<int>(entries.size()) - left_count;

  page_id_t parent_id = Hdr(p)->parent_page_id;
  page_id_t old_next = Hdr(p)->next_leaf_page_id;

  page_id_t right_id;
  Page* right = bpm_->NewPage(&right_id);
  if (right == nullptr) { bpm_->UnpinPage(leaf_id, false); return Status::OutOfSpace("buffer pool exhausted during split"); }
  InitLeaf(right, parent_id);
  for (int i = 0; i < right_count; ++i) {
    SetKeyAt(right, i, true, entries[left_count + i].first);
    SetRidAt(right, i, entries[left_count + i].second);
  }
  Hdr(right)->num_keys = static_cast<int16_t>(right_count);
  Hdr(right)->next_leaf_page_id = old_next;

  // Rewrite left (original) leaf with just the first half.
  for (int i = 0; i < left_count; ++i) {
    SetKeyAt(p, i, true, entries[i].first);
    SetRidAt(p, i, entries[i].second);
  }
  Hdr(p)->num_keys = static_cast<int16_t>(left_count);
  Hdr(p)->next_leaf_page_id = right_id;

  std::string sep_key = entries[left_count].first;

  bpm_->UnpinPage(leaf_id, true);
  bpm_->UnpinPage(right_id, true);

  InsertIntoParentAfterSplit(leaf_id, sep_key, right_id);
  return Status::OK();
}

void BPlusTree::InsertIntoParentAfterSplit(page_id_t left_id, const std::string& sep_key, page_id_t right_id) {
  Page* left = bpm_->FetchPage(left_id);
  page_id_t parent_id = Hdr(left)->parent_page_id;
  bpm_->UnpinPage(left_id, false);

  if (parent_id == kInvalidPageId) {
    // left was the root: create a new root above both halves.
    page_id_t new_root_id;
    Page* root = bpm_->NewPage(&new_root_id);
    InitInternal(root, kInvalidPageId);
    SetChildAt(root, 0, left_id);
    SetKeyAt(root, 0, false, sep_key);
    SetChildAt(root, 1, right_id);
    Hdr(root)->num_keys = 1;
    bpm_->UnpinPage(new_root_id, true);

    Page* l = bpm_->FetchPage(left_id);
    Hdr(l)->parent_page_id = new_root_id;
    bpm_->UnpinPage(left_id, true);
    Page* r = bpm_->FetchPage(right_id);
    Hdr(r)->parent_page_id = new_root_id;
    bpm_->UnpinPage(right_id, true);

    root_page_id_ = new_root_id;
    return;
  }

  Page* parent = bpm_->FetchPage(parent_id);
  int n = NumKeys(parent);

  int idx = n;
  for (int i = 0; i < n; ++i) {
    if (sep_key < GetKeyAt(parent, i, false)) { idx = i; break; }
  }

  if (n < kMaxKeysPerNode) {
    for (int i = n; i > idx; --i) {
      SetKeyAt(parent, i, false, GetKeyAt(parent, i - 1, false));
    }
    for (int i = n + 1; i > idx + 1; --i) {
      SetChildAt(parent, i, GetChildAt(parent, i - 1));
    }
    SetKeyAt(parent, idx, false, sep_key);
    SetChildAt(parent, idx + 1, right_id);
    Hdr(parent)->num_keys = static_cast<int16_t>(n + 1);
    bpm_->UnpinPage(parent_id, true);

    Page* r = bpm_->FetchPage(right_id);
    Hdr(r)->parent_page_id = parent_id;
    bpm_->UnpinPage(right_id, true);
    return;
  }

  // Parent (internal node) is full too: split it, promoting the median key.
  std::vector<std::string> keys;
  std::vector<page_id_t> children;
  keys.reserve(n + 1);
  children.reserve(n + 2);
  for (int i = 0; i < n; ++i) keys.push_back(GetKeyAt(parent, i, false));
  for (int i = 0; i <= n; ++i) children.push_back(GetChildAt(parent, i));

  keys.insert(keys.begin() + idx, sep_key);
  children.insert(children.begin() + idx + 1, right_id);

  int mid = static_cast<int>(keys.size()) / 2;
  std::string promoted = keys[mid];

  page_id_t grandparent_id = Hdr(parent)->parent_page_id;

  page_id_t new_right_id;
  Page* new_right = bpm_->NewPage(&new_right_id);
  InitInternal(new_right, grandparent_id);
  int rk = 0;
  for (size_t i = mid + 1; i < keys.size(); ++i) SetKeyAt(new_right, rk++, false, keys[i]);
  int rc = 0;
  for (size_t i = mid + 1; i < children.size(); ++i) SetChildAt(new_right, rc++, children[i]);
  Hdr(new_right)->num_keys = static_cast<int16_t>(rk);

  // Reparent children moved into new_right.
  for (int i = 0; i < rc; ++i) {
    Page* c = bpm_->FetchPage(GetChildAt(new_right, i));
    Hdr(c)->parent_page_id = new_right_id;
    bpm_->UnpinPage(GetChildAt(new_right, i), true);
  }

  // Rewrite parent (now "left half") with keys[0..mid) and children[0..mid].
  for (int i = 0; i < mid; ++i) SetKeyAt(parent, i, false, keys[static_cast<size_t>(i)]);
  for (int i = 0; i <= mid; ++i) SetChildAt(parent, i, children[static_cast<size_t>(i)]);
  Hdr(parent)->num_keys = static_cast<int16_t>(mid);

  bpm_->UnpinPage(parent_id, true);
  bpm_->UnpinPage(new_right_id, true);

  InsertIntoParentAfterSplit(parent_id, promoted, new_right_id);
}

bool BPlusTree::Remove(const std::string& key) {
  std::lock_guard<std::mutex> lock(tree_mu_);
  page_id_t leaf_id = FindLeaf(key);
  if (leaf_id == kInvalidPageId) return false;
  Page* p = bpm_->FetchPage(leaf_id);
  if (p == nullptr) return false;
  int n = NumKeys(p);
  int found = -1;
  for (int i = 0; i < n; ++i) {
    if (GetKeyAt(p, i, true) == key) { found = i; break; }
  }
  if (found < 0) {
    bpm_->UnpinPage(leaf_id, false);
    return false;
  }
  for (int i = found; i < n - 1; ++i) {
    SetKeyAt(p, i, true, GetKeyAt(p, i + 1, true));
    SetRidAt(p, i, GetRidAt(p, i + 1));
  }
  Hdr(p)->num_keys = static_cast<int16_t>(n - 1);
  bpm_->UnpinPage(leaf_id, true);
  return true;
}

std::vector<std::pair<std::string, RID>> BPlusTree::Scan(const std::string& start_key, size_t limit) const {
  std::lock_guard<std::mutex> lock(tree_mu_);
  std::vector<std::pair<std::string, RID>> results;
  page_id_t leaf_id = FindLeaf(start_key);
  if (leaf_id == kInvalidPageId) return results;

  while (leaf_id != kInvalidPageId) {
    Page* p = bpm_->FetchPage(leaf_id);
    if (p == nullptr) break;
    int n = NumKeys(p);
    for (int i = 0; i < n; ++i) {
      std::string k = GetKeyAt(p, i, true);
      if (k >= start_key) {
        results.emplace_back(k, GetRidAt(p, i));
        if (limit != 0 && results.size() >= limit) {
          bpm_->UnpinPage(leaf_id, false);
          return results;
        }
      }
    }
    page_id_t next = Hdr(p)->next_leaf_page_id;
    bpm_->UnpinPage(leaf_id, false);
    leaf_id = next;
  }
  return results;
}

}  // namespace desentry
