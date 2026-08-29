#pragma once
// Slotted page record layout -- the same physical technique Postgres heap
// pages and most B+Tree leaf pages use: a small header + a slot directory
// that grows forward from just after the header, while record bytes grow
// backward from the end of the page. This lets variable-length records
// (a tiny structured row next to a large unstructured JSON blob) share a
// page efficiently, and lets a record be deleted (tombstoned) without
// having to shift every other record's offset.
//
// Layout of one page's kPageSize bytes:
//   [ PageHeader | slot[0] | slot[1] | ... |  ...free... | rec[1] | rec[0] ]
//                 ^ grows right                           ^ grows left
//
// A deleted slot has length == kTombstoneLength; its bytes are not reclaimed
// until a (future, roadmap-noted) page compaction pass -- exactly the
// "vacuum later" trade-off real engines make rather than paying a
// compaction cost on every delete.

#include <cstdint>
#include <string>

#include "desentry/storage/page.h"

namespace desentry {

constexpr uint16_t kTombstoneLength = 0xFFFF;

#pragma pack(push, 1)
struct SlottedPageHeader {
  uint16_t num_slots;
  uint16_t free_space_offset;  // offset (from page start) where free space ends / record data begins
};
struct SlotEntry {
  uint16_t offset;  // offset from page start where this record's bytes begin
  uint16_t length;  // record length in bytes; kTombstoneLength => deleted
};
#pragma pack(pop)

class SlottedPage {
 public:
  static void Init(Page* page);

  // Inserts `data`; returns the new slot id, or -1 if the page doesn't have
  // enough contiguous free space (caller should allocate a new page).
  static slot_id_t InsertRecord(Page* page, const std::string& data);

  // Returns false if the slot is out of range or tombstoned.
  static bool GetRecord(const Page* page, slot_id_t slot_id, std::string* out);

  static bool DeleteRecord(Page* page, slot_id_t slot_id);

  static uint16_t NumSlots(const Page* page);
  static size_t FreeSpace(const Page* page);

 private:
  static SlottedPageHeader* Header(Page* page);
  static const SlottedPageHeader* Header(const Page* page);
  static SlotEntry* Slot(Page* page, slot_id_t slot_id);
  static const SlotEntry* Slot(const Page* page, slot_id_t slot_id);
};

}  // namespace desentry
