#include "desentry/storage/slotted_page.h"

#include <cstring>

namespace desentry {

SlottedPageHeader* SlottedPage::Header(Page* page) {
  return reinterpret_cast<SlottedPageHeader*>(page->GetData());
}
const SlottedPageHeader* SlottedPage::Header(const Page* page) {
  return reinterpret_cast<const SlottedPageHeader*>(page->GetData());
}
SlotEntry* SlottedPage::Slot(Page* page, slot_id_t slot_id) {
  return reinterpret_cast<SlotEntry*>(page->GetData() + sizeof(SlottedPageHeader) +
                                       static_cast<size_t>(slot_id) * sizeof(SlotEntry));
}
const SlotEntry* SlottedPage::Slot(const Page* page, slot_id_t slot_id) {
  return reinterpret_cast<const SlotEntry*>(page->GetData() + sizeof(SlottedPageHeader) +
                                             static_cast<size_t>(slot_id) * sizeof(SlotEntry));
}

void SlottedPage::Init(Page* page) {
  SlottedPageHeader* h = Header(page);
  h->num_slots = 0;
  h->free_space_offset = static_cast<uint16_t>(kPageSize);
}

size_t SlottedPage::FreeSpace(const Page* page) {
  const SlottedPageHeader* h = Header(page);
  size_t slots_end = sizeof(SlottedPageHeader) + static_cast<size_t>(h->num_slots) * sizeof(SlotEntry);
  if (slots_end > h->free_space_offset) return 0;
  return h->free_space_offset - slots_end;
}

uint16_t SlottedPage::NumSlots(const Page* page) { return Header(page)->num_slots; }

slot_id_t SlottedPage::InsertRecord(Page* page, const std::string& data) {
  SlottedPageHeader* h = Header(page);
  size_t needed = data.size() + sizeof(SlotEntry);
  if (FreeSpace(page) < needed) return -1;

  // Try to reuse a tombstoned slot first to keep the slot array from
  // growing unboundedly under update-heavy workloads.
  for (uint16_t i = 0; i < h->num_slots; ++i) {
    SlotEntry* s = Slot(page, static_cast<slot_id_t>(i));
    if (s->length == kTombstoneLength) {
      // Reusing an existing slot entry costs only the record bytes, which
      // the caller's FreeSpace() check above (computed against the more
      // conservative "brand new slot" cost) already covers.
      uint16_t new_offset = static_cast<uint16_t>(h->free_space_offset - data.size());
      std::memcpy(page->GetData() + new_offset, data.data(), data.size());
      s->offset = new_offset;
      s->length = static_cast<uint16_t>(data.size());
      h->free_space_offset = new_offset;
      return static_cast<slot_id_t>(i);
    }
  }

  uint16_t new_offset = static_cast<uint16_t>(h->free_space_offset - data.size());
  std::memcpy(page->GetData() + new_offset, data.data(), data.size());
  SlotEntry* s = Slot(page, static_cast<slot_id_t>(h->num_slots));
  s->offset = new_offset;
  s->length = static_cast<uint16_t>(data.size());
  h->free_space_offset = new_offset;
  slot_id_t id = static_cast<slot_id_t>(h->num_slots);
  h->num_slots++;
  return id;
}

bool SlottedPage::GetRecord(const Page* page, slot_id_t slot_id, std::string* out) {
  const SlottedPageHeader* h = Header(page);
  if (slot_id < 0 || slot_id >= static_cast<slot_id_t>(h->num_slots)) return false;
  const SlotEntry* s = Slot(page, slot_id);
  if (s->length == kTombstoneLength) return false;
  out->assign(page->GetData() + s->offset, s->length);
  return true;
}

bool SlottedPage::DeleteRecord(Page* page, slot_id_t slot_id) {
  SlottedPageHeader* h = Header(page);
  if (slot_id < 0 || slot_id >= static_cast<slot_id_t>(h->num_slots)) return false;
  SlotEntry* s = Slot(page, slot_id);
  if (s->length == kTombstoneLength) return false;
  s->length = kTombstoneLength;
  s->offset = 0;
  return true;
}

}  // namespace desentry
