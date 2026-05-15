#include "page/bitmap_page.h"

#include "glog/logging.h"

template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  size_t max_pages = GetMaxSupportedSize();
  if (page_allocated_ >= max_pages) {
    return false;
  }
  for (size_t i = 0; i < max_pages; i++) {
    size_t idx = (next_free_page_ + i) % max_pages;
    size_t byte_index = idx / 8;
    uint8_t bit_index = idx % 8;
    if (IsPageFreeLow(byte_index, bit_index)) {
      bytes[byte_index] |= (1 << bit_index);
      page_allocated_++;
      next_free_page_ = (idx + 1) % max_pages;
      page_offset = idx;
      return true;
    }
  }
  return false;
}

template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
  size_t max_pages = GetMaxSupportedSize();
  if (page_offset >= max_pages) {
    return false;
  }
  size_t byte_index = page_offset / 8;
  uint8_t bit_index = page_offset % 8;
  if (IsPageFreeLow(byte_index, bit_index)) {
    return false;
  }
  bytes[byte_index] &= ~(1 << bit_index);
  page_allocated_--;
  next_free_page_ = page_offset;
  return true;
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  size_t max_pages = GetMaxSupportedSize();
  if (page_offset >= max_pages) {
    return false;
  }
  return IsPageFreeLow(page_offset / 8, page_offset % 8);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  return (bytes[byte_index] & (1 << bit_index)) == 0;
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;