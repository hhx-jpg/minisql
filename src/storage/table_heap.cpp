#include "storage/table_heap.h"

#include "page/table_page.h"

/*****************************************************************************
 * TableHeap constructor for creating a NEW table
 *****************************************************************************/
// Defined inline in header (line 110-117). The current code has an ASSERT(false).
// We fix it by editing the header — the constructor should allocate the first page.

bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  uint32_t serialized_size = row.GetSerializedSize(schema_);
  if (serialized_size >= TablePage::SIZE_MAX_ROW) {
    return false;
  }

  // Try existing pages
  page_id_t current_page_id = first_page_id_;
  while (current_page_id != INVALID_PAGE_ID) {
    auto *page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(current_page_id));
    if (page == nullptr) {
      return false;
    }
    if (page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      buffer_pool_manager_->UnpinPage(current_page_id, true);
      return true;
    }
    page_id_t next_page_id = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    current_page_id = next_page_id;
  }

  // No space — allocate a new page
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    return false;
  }

  auto *table_page = reinterpret_cast<TablePage *>(new_page->GetData());
  table_page->Init(new_page_id, INVALID_PAGE_ID, log_manager_, txn);

  // Link: find the last page and set its NextPageId to the new page
  current_page_id = first_page_id_;
  while (true) {
    auto *page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(current_page_id));
    page_id_t next = page->GetNextPageId();
    if (next == INVALID_PAGE_ID) {
      page->SetNextPageId(new_page_id);
      table_page->SetPrevPageId(current_page_id);
      buffer_pool_manager_->UnpinPage(current_page_id, true);
      break;
    }
    buffer_pool_manager_->UnpinPage(current_page_id, false);
    current_page_id = next;
  }

  table_page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);
  buffer_pool_manager_->UnpinPage(new_page_id, true);
  return true;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return false;
  }
  page->WLatch();
  page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
  return true;
}

bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto *page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return false;
  }

  Row old_row(rid);
  bool updated = page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_);

  if (!updated) {
    // Not enough space in current page — delete and re-insert
    page->MarkDelete(rid, txn, lock_manager_, log_manager_);
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);

    if (!InsertTuple(row, txn)) {
      return false;
    }
  } else {
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
  }

  return true;
}

void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) return;
  page->WLatch();
  page->ApplyDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

bool TableHeap::GetTuple(Row *row, Txn *txn) {
  auto *page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) {
    return false;
  }
  bool found = page->GetTuple(row, schema_, txn, lock_manager_);
  buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
  return found;
}

void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

TableIterator TableHeap::Begin(Txn *txn) {
  // Find the first valid tuple
  page_id_t page_id = first_page_id_;
  while (page_id != INVALID_PAGE_ID) {
    auto *page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));
    RowId rid;
    if (page->GetFirstTupleRid(&rid)) {
      buffer_pool_manager_->UnpinPage(page_id, false);
      return TableIterator(this, rid, txn);
    }
    page_id_t next = page->GetNextPageId();
    buffer_pool_manager_->UnpinPage(page_id, false);
    page_id = next;
  }
  return End();
}

TableIterator TableHeap::End() {
  return TableIterator(nullptr, RowId(INVALID_PAGE_ID), nullptr);
}
