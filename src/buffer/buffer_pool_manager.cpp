#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  lock_guard<recursive_mutex> lock(latch_);

  // 1. Search the page table for the requested page.
  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    page->pin_count_++;
    replacer_->Pin(frame_id);
    return page;
  }

  // 1.2 Page not in memory — find a replacement frame.
  frame_id_t frame_id;
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else if (!replacer_->Victim(&frame_id)) {
    return nullptr;
  }

  Page *page = &pages_[frame_id];

  // 2. If the replacement page is dirty, write it back to disk.
  if (page->IsDirty()) {
    disk_manager_->WritePage(page->page_id_, page->data_);
  }

  // 3. Delete old page from page table and insert new mapping.
  if (page->page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(page->page_id_);
  }
  page_table_[page_id] = frame_id;

  // 4. Update metadata, read page content from disk.
  page->page_id_ = page_id;
  page->pin_count_ = 1;
  page->is_dirty_ = false;
  disk_manager_->ReadPage(page_id, page->data_);

  return page;
}

Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  lock_guard<recursive_mutex> lock(latch_);

  // 0. Pick a victim frame: free list first, then replacer.
  frame_id_t frame_id;
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else if (!replacer_->Victim(&frame_id)) {
    // All pages are pinned.
    return nullptr;
  }

  // 1. Allocate a new page id on disk.
  page_id = AllocatePage();

  Page *page = &pages_[frame_id];

  // Write back dirty page before replacing.
  if (page->IsDirty()) {
    disk_manager_->WritePage(page->page_id_, page->data_);
  }

  // Remove old mapping.
  if (page->page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(page->page_id_);
  }

  // 3. Reset metadata, zero out memory, add to page table.
  page->page_id_ = page_id;
  page->pin_count_ = 1;
  page->is_dirty_ = false;
  page->ResetMemory();
  page_table_[page_id] = frame_id;

  return page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
  lock_guard<recursive_mutex> lock(latch_);

  // 0. Deallocate on disk.
  DeallocatePage(page_id);

  // 1. Search the page table.
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return true;
  }

  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];

  // 2. If pin_count > 0, someone is using the page.
  if (page->pin_count_ > 0) {
    return false;
  }

  // 3. Remove from page table and replacer, reset metadata, return to free list.
  replacer_->Pin(frame_id);
  page_table_.erase(it);
  page->page_id_ = INVALID_PAGE_ID;
  page->pin_count_ = 0;
  page->is_dirty_ = false;
  page->ResetMemory();
  free_list_.push_back(frame_id);

  return true;
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  lock_guard<recursive_mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];

  if (page->pin_count_ <= 0) {
    return false;
  }

  page->pin_count_--;
  if (is_dirty) {
    page->is_dirty_ = true;
  }

  if (page->pin_count_ == 0) {
    replacer_->Unpin(frame_id);
  }

  return true;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
  lock_guard<recursive_mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  Page *page = &pages_[it->second];
  if (page->IsDirty()) {
    disk_manager_->WritePage(page_id, page->data_);
    page->is_dirty_ = false;
  }

  return true;
}

page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

frame_id_t BufferPoolManager::TryToFindFreePage() {
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ == 0) {
      return static_cast<frame_id_t>(i);
    }
  }
  return INVALID_FRAME_ID;
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}
