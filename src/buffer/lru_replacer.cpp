#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) {}

LRUReplacer::~LRUReplacer() = default;

bool LRUReplacer::Victim(frame_id_t *frame_id) {
  lock_guard<mutex> lock(latch_);
  if (lru_list_.empty()) {
    return false;
  }
  *frame_id = lru_list_.back();
  lru_map_.erase(*frame_id);
  lru_list_.pop_back();
  return true;
}

void LRUReplacer::Pin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  auto it = lru_map_.find(frame_id);
  if (it != lru_map_.end()) {
    lru_list_.erase(it->second);
    lru_map_.erase(it);
  }
}

void LRUReplacer::Unpin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  if (lru_map_.find(frame_id) != lru_map_.end()) {
    return;
  }
  lru_list_.push_front(frame_id);
  lru_map_[frame_id] = lru_list_.begin();
}

size_t LRUReplacer::Size() {
  lock_guard<mutex> lock(latch_);
  return lru_list_.size();
}
