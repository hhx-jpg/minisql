#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()

void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  if (max_size == UNDEFINED_SIZE) {
    max_size = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (key_size + sizeof(page_id_t)) - 1;
  }
  SetMaxSize(max_size);
}

GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value) return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(page_id_t)));
}

/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
  // Binary search. Keys are at indices 1..size-1 (first key is dummy).
  // Find the child pointer s.t. Key(i) <= key < Key(i+1).
  // Return ValueAt(0) if key < KeyAt(1).
  int left = 1;
  int right = GetSize() - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    if (KM.CompareKeys(key, KeyAt(mid)) < 0) {
      right = mid - 1;
    } else {
      left = mid + 1;
    }
  }
  return ValueAt(left - 1);
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key,
                                   const page_id_t &new_value) {
  SetValueAt(0, old_value);
  SetKeyAt(1, new_key);
  SetValueAt(1, new_value);
  SetSize(2);
}

int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key,
                                  const page_id_t &new_value) {
  int idx = ValueIndex(old_value) + 1;
  int sz = GetSize();
  // Shift elements >= idx to the right by 1
  for (int i = sz; i > idx; i--) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetKeyAt(idx, new_key);
  SetValueAt(idx, new_value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
  int half = GetSize() / 2;
  int move_start = GetSize() - half;
  recipient->CopyNFrom(PairPtrAt(move_start), half, buffer_pool_manager);
  SetSize(move_start);
}

void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
  PairCopy(PairPtrAt(GetSize()), src, size);
  // Update parent page id for each moved child page
  for (int i = 0; i < size; i++) {
    page_id_t child_id = ValueAt(GetSize() + i);
    auto *child_page = buffer_pool_manager->FetchPage(child_id);
    if (child_page != nullptr) {
      auto *bp_page = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
      bp_page->SetParentPageId(GetPageId());
      buffer_pool_manager->UnpinPage(child_id, true);
    }
  }
  IncreaseSize(size);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
void InternalPage::Remove(int index) {
  int sz = GetSize();
  for (int i = index; i < sz - 1; i++) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  IncreaseSize(-1);
}

page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  page_id_t child = ValueAt(0);
  SetSize(0);
  return child;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key,
                             BufferPoolManager *buffer_pool_manager) {
  // recipient's last key becomes middle_key, then copy our pairs
  recipient->CopyLastFrom(middle_key, ValueAt(0), buffer_pool_manager);
  recipient->CopyNFrom(PairPtrAt(1), GetSize() - 1, buffer_pool_manager);
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
void InternalPage::MoveFirstToEndOf(InternalPage *recipient, GenericKey *middle_key,
                                    BufferPoolManager *buffer_pool_manager) {
  // Our first child (ValueAt(0)) moves to the end of recipient.
  // The key between us and recipient is middle_key.
  recipient->CopyLastFrom(middle_key, ValueAt(0), buffer_pool_manager);
  // Then shift: the dummy key becomes KeyAt(1), etc.
  Remove(0);
}

void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value,
                                BufferPoolManager *buffer_pool_manager) {
  int sz = GetSize();
  SetKeyAt(sz, key);
  SetValueAt(sz, value);
  IncreaseSize(1);
  // Update the child's parent
  auto *child_page = buffer_pool_manager->FetchPage(value);
  if (child_page != nullptr) {
    auto *bp_page = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    bp_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(value, true);
  }
}

void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {
  // Our last key-value pair goes to front of recipient (as first real entry, after dummy).
  int last = GetSize() - 1;
  recipient->CopyFirstFrom(ValueAt(last), buffer_pool_manager);
  // The middle_key becomes recipient's first real key (at index 1)
  recipient->SetKeyAt(1, middle_key);
  // Remove our last entry
  IncreaseSize(-1);
}

void InternalPage::CopyFirstFrom(const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  int sz = GetSize();
  // Shift everything right by 1
  for (int i = sz; i > 0; i--) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetValueAt(0, value);
  IncreaseSize(1);
  // Update the child's parent
  auto *child_page = buffer_pool_manager->FetchPage(value);
  if (child_page != nullptr) {
    auto *bp_page = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    bp_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(value, true);
  }
}
