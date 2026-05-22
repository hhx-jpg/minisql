#include "storage/table_iterator.h"

#include "common/macros.h"
#include "page/table_page.h"
#include "storage/table_heap.h"

TableIterator::TableIterator(TableHeap *table_heap, RowId rid, Txn *txn)
    : table_heap_(table_heap), cur_rid_(rid), txn_(txn) {}

TableIterator::TableIterator(const TableIterator &other)
    : table_heap_(other.table_heap_), cur_rid_(other.cur_rid_), row_(other.row_), txn_(other.txn_) {}

TableIterator::~TableIterator() {}

bool TableIterator::operator==(const TableIterator &itr) const {
  return cur_rid_.Get() == itr.cur_rid_.Get();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr);
}

const Row &TableIterator::operator*() {
  row_.SetRowId(cur_rid_);
  table_heap_->GetTuple(&row_, txn_);
  return row_;
}

Row *TableIterator::operator->() {
  row_.SetRowId(cur_rid_);
  table_heap_->GetTuple(&row_, txn_);
  return &row_;
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  table_heap_ = itr.table_heap_;
  cur_rid_ = itr.cur_rid_;
  txn_ = itr.txn_;
  row_ = itr.row_;
  return *this;
}

TableIterator &TableIterator::operator++() {
  auto *page = reinterpret_cast<TablePage *>(
      table_heap_->buffer_pool_manager_->FetchPage(cur_rid_.GetPageId()));

  RowId next_rid;
  if (page->GetNextTupleRid(cur_rid_, &next_rid)) {
    // Found next tuple in same page
    cur_rid_ = next_rid;
    table_heap_->buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);
    return *this;
  }

  // Need to move to next page
  page_id_t next_page_id = page->GetNextPageId();
  table_heap_->buffer_pool_manager_->UnpinPage(page->GetTablePageId(), false);

  while (next_page_id != INVALID_PAGE_ID) {
    auto *next_page = reinterpret_cast<TablePage *>(
        table_heap_->buffer_pool_manager_->FetchPage(next_page_id));
    if (next_page->GetFirstTupleRid(&next_rid)) {
      cur_rid_ = next_rid;
      table_heap_->buffer_pool_manager_->UnpinPage(next_page_id, false);
      return *this;
    }
    page_id_t temp = next_page->GetNextPageId();
    table_heap_->buffer_pool_manager_->UnpinPage(next_page_id, false);
    next_page_id = temp;
  }

  // Reached end
  cur_rid_ = RowId(INVALID_PAGE_ID);
  return *this;
}

TableIterator TableIterator::operator++(int) {
  RowId saved_rid = cur_rid_;
  ++(*this);
  return TableIterator(table_heap_, saved_rid, txn_);
}
