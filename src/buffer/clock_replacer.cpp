#include "buffer/clock_replacer.h"

CLOCKReplacer::CLOCKReplacer(size_t num_pages)
    : capacity_(num_pages), hand_(0), size_(0), in_replacer_(num_pages, false), ref_bit_(num_pages, false) {}

CLOCKReplacer::~CLOCKReplacer() = default;

bool CLOCKReplacer::Victim(frame_id_t *frame_id) {
  lock_guard<mutex> lock(latch_);
  if (size_ == 0 || capacity_ == 0) {
    return false;
  }

  while (true) {
    if (in_replacer_[hand_]) {
      if (ref_bit_[hand_]) {
        ref_bit_[hand_] = false;
      } else {
        *frame_id = static_cast<frame_id_t>(hand_);
        in_replacer_[hand_] = false;
        size_--;
        hand_ = (hand_ + 1) % capacity_;
        return true;
      }
    }
    hand_ = (hand_ + 1) % capacity_;
  }
}

void CLOCKReplacer::Pin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= capacity_) {
    return;
  }

  size_t index = static_cast<size_t>(frame_id);
  if (!in_replacer_[index]) {
    return;
  }
  in_replacer_[index] = false;
  ref_bit_[index] = false;
  size_--;
}

void CLOCKReplacer::Unpin(frame_id_t frame_id) {
  lock_guard<mutex> lock(latch_);
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= capacity_) {
    return;
  }

  size_t index = static_cast<size_t>(frame_id);
  ref_bit_[index] = true;
  if (!in_replacer_[index]) {
    in_replacer_[index] = true;
    size_++;
  }
}

size_t CLOCKReplacer::Size() {
  lock_guard<mutex> lock(latch_);
  return size_;
}
