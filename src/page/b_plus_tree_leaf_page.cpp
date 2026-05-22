#include "page/b_plus_tree_leaf_page.h"

#include <algorithm>

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(RowId))
#define key_off 0
#define val_off GetKeySize()

void LeafPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetNextPageId(INVALID_PAGE_ID);
  SetKeySize(key_size);
  if (max_size == UNDEFINED_SIZE) {
    max_size = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (key_size + sizeof(RowId)) - 1;
  }
  SetMaxSize(max_size);
}

page_id_t LeafPage::GetNextPageId() const {
  return next_page_id_;
}

void LeafPage::SetNextPageId(page_id_t next_page_id) {
  next_page_id_ = next_page_id;
}

int LeafPage::KeyIndex(const GenericKey *key, const KeyManager &KM) {
  // Binary search for first index i where KeyAt(i) >= key
  int left = 0;
  int right = GetSize() - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    if (KM.CompareKeys(key, KeyAt(mid)) <= 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  return left;
}

GenericKey *LeafPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void LeafPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

RowId LeafPage::ValueAt(int index) const {
  return *reinterpret_cast<const RowId *>(pairs_off + index * pair_size + val_off);
}

void LeafPage::SetValueAt(int index, RowId value) {
  *reinterpret_cast<RowId *>(pairs_off + index * pair_size + val_off) = value;
}

void *LeafPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void LeafPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(RowId)));
}

std::pair<GenericKey *, RowId> LeafPage::GetItem(int index) {
  return {KeyAt(index), ValueAt(index)};
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
int LeafPage::Insert(GenericKey *key, const RowId &value, const KeyManager &KM) {
  int idx = KeyIndex(key, KM);
  int sz = GetSize();

  // Duplicate check
  if (idx < sz && KM.CompareKeys(key, KeyAt(idx)) == 0) {
    return -1;  // duplicate key
  }

  // Shift elements >= idx to the right
  for (int i = sz; i > idx; i--) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetKeyAt(idx, key);
  SetValueAt(idx, value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
void LeafPage::MoveHalfTo(LeafPage *recipient) {
  int half = GetSize() / 2;
  int move_start = GetSize() - half;
  recipient->CopyNFrom(PairPtrAt(move_start), half);
  SetSize(move_start);
}

void LeafPage::CopyNFrom(void *src, int size) {
  PairCopy(PairPtrAt(GetSize()), src, size);
  IncreaseSize(size);
}

/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
bool LeafPage::Lookup(const GenericKey *key, RowId &value, const KeyManager &KM) {
  int idx = KeyIndex(key, KM);
  if (idx < GetSize() && KM.CompareKeys(key, KeyAt(idx)) == 0) {
    value = ValueAt(idx);
    return true;
  }
  return false;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
int LeafPage::RemoveAndDeleteRecord(const GenericKey *key, const KeyManager &KM) {
  int idx = KeyIndex(key, KM);
  if (idx >= GetSize() || KM.CompareKeys(key, KeyAt(idx)) != 0) {
    return GetSize();  // not found, no change
  }
  // Shift elements left
  for (int i = idx; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
  return GetSize();
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
void LeafPage::MoveAllTo(LeafPage *recipient) {
  recipient->CopyNFrom(PairPtrAt(0), GetSize());
  recipient->SetNextPageId(GetNextPageId());
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
void LeafPage::MoveFirstToEndOf(LeafPage *recipient) {
  recipient->CopyLastFrom(KeyAt(0), ValueAt(0));
  // Shift our remaining elements left
  for (int i = 0; i < GetSize() - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
}

void LeafPage::CopyLastFrom(GenericKey *key, const RowId value) {
  int sz = GetSize();
  SetKeyAt(sz, key);
  SetValueAt(sz, value);
  IncreaseSize(1);
}

void LeafPage::MoveLastToFrontOf(LeafPage *recipient) {
  int last = GetSize() - 1;
  recipient->CopyFirstFrom(KeyAt(last), ValueAt(last));
  IncreaseSize(-1);
}

void LeafPage::CopyFirstFrom(GenericKey *key, const RowId value) {
  // Shift everything right by 1
  for (int i = GetSize(); i > 0; i--) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetKeyAt(0, key);
  SetValueAt(0, value);
  IncreaseSize(1);
}
