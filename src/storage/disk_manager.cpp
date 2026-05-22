#include "storage/disk_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <stdexcept>

#include "glog/logging.h"
#include "page/bitmap_page.h"

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  // directory or file does not exist
  if (!db_io_.is_open()) {
    db_io_.clear();
    // create a new file
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
    db_io_.close();
    // reopen with original mode
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      throw std::exception();
    }
  }
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
}

void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  if (!closed) {
    db_io_.close();
    closed = true;
  }
}

void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

page_id_t DiskManager::AllocatePage() {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  // Search existing extents for a free page
  for (uint32_t i = 0; i < meta->num_extents_; i++) {
    if (meta->extent_used_page_[i] >= BITMAP_SIZE) continue;

    page_id_t bitmap_phys_id = 1 + i * (1 + BITMAP_SIZE);
    char bitmap_buf[PAGE_SIZE];
    ReadPhysicalPage(bitmap_phys_id, bitmap_buf);
    auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buf);

    uint32_t page_offset;
    if (bitmap->AllocatePage(page_offset)) {
      WritePhysicalPage(bitmap_phys_id, bitmap_buf);
      meta->num_allocated_pages_++;
      meta->extent_used_page_[i]++;
      WritePhysicalPage(META_PAGE_ID, meta_data_);
      return static_cast<page_id_t>(i * BITMAP_SIZE + page_offset);
    }
  }

  // No free page in existing extents — create a new extent
  uint32_t new_extent_idx = meta->num_extents_;
  page_id_t bitmap_phys_id = 1 + new_extent_idx * (1 + BITMAP_SIZE);

  char bitmap_buf[PAGE_SIZE];
  memset(bitmap_buf, 0, PAGE_SIZE);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buf);

  uint32_t page_offset;
  bool success = bitmap->AllocatePage(page_offset);
  ASSERT(success, "New bitmap page should have free pages.");

  WritePhysicalPage(bitmap_phys_id, bitmap_buf);

  // Initialize the corresponding data page to zero
  page_id_t data_phys_id = bitmap_phys_id + 1 + page_offset;
  char data_page[PAGE_SIZE];
  memset(data_page, 0, PAGE_SIZE);
  WritePhysicalPage(data_phys_id, data_page);

  meta->num_extents_++;
  meta->num_allocated_pages_++;
  meta->extent_used_page_[new_extent_idx] = 1;
  WritePhysicalPage(META_PAGE_ID, meta_data_);

  return static_cast<page_id_t>(new_extent_idx * BITMAP_SIZE + page_offset);
}

void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  size_t extent_idx = logical_page_id / BITMAP_SIZE;
  ASSERT(extent_idx < meta->num_extents_, "Cannot deallocate page from non-existent extent.");

  page_id_t bitmap_phys_id = 1 + extent_idx * (1 + BITMAP_SIZE);
  char bitmap_buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_phys_id, bitmap_buf);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buf);

  uint32_t page_offset = logical_page_id % BITMAP_SIZE;
  bool success = bitmap->DeAllocatePage(page_offset);
  ASSERT(success, "Page is already free.");

  WritePhysicalPage(bitmap_phys_id, bitmap_buf);
  meta->num_allocated_pages_--;
  meta->extent_used_page_[extent_idx]--;
  WritePhysicalPage(META_PAGE_ID, meta_data_);
}

bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  auto *meta = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  size_t extent_idx = logical_page_id / BITMAP_SIZE;
  if (extent_idx >= meta->num_extents_) {
    return true;
  }

  page_id_t bitmap_phys_id = 1 + extent_idx * (1 + BITMAP_SIZE);
  char bitmap_buf[PAGE_SIZE];
  ReadPhysicalPage(bitmap_phys_id, bitmap_buf);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buf);

  return bitmap->IsPageFree(logical_page_id % BITMAP_SIZE);
}

page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  size_t extent_idx = logical_page_id / BITMAP_SIZE;
  size_t offset_in_extent = logical_page_id % BITMAP_SIZE;
  // physical layout: Meta | Bitmap0 | Page0..Page{N-1} | Bitmap1 | PageN..Page{2N-1} | ...
  return 2 + extent_idx * (1 + BITMAP_SIZE) + offset_in_extent;
}

int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // check if read beyond file length
  if (offset >= GetFileSize(file_name_)) {
#ifdef ENABLE_BPM_DEBUG
    LOG(INFO) << "Read less than a page" << std::endl;
#endif
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // set read cursor to offset
    db_io_.seekp(offset);
    db_io_.read(page_data, PAGE_SIZE);
    // if file ends before reading PAGE_SIZE
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
#ifdef ENABLE_BPM_DEBUG
      LOG(INFO) << "Read less than a page" << std::endl;
#endif
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // set write cursor to offset
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);
  // check for I/O error
  if (db_io_.bad()) {
    LOG(ERROR) << "I/O error while writing";
    return;
  }
  // needs to flush to keep disk file in sync
  db_io_.flush();
}