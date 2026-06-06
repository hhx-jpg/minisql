#ifndef MINISQL_CLOCK_REPLACER_H
#define MINISQL_CLOCK_REPLACER_H

#include <mutex>
#include <vector>

#include "buffer/replacer.h"
#include "common/config.h"

using namespace std;

/**
 * CLOCKReplacer implements the clock replacement.
 */
class CLOCKReplacer : public Replacer {
 public:
  /**
   * Create a new CLOCKReplacer.
   * @param num_pages the maximum number of pages the CLOCKReplacer will be required to store
   */
  explicit CLOCKReplacer(size_t num_pages);

  /**
   * Destroys the CLOCKReplacer.
   */
  ~CLOCKReplacer() override;

  bool Victim(frame_id_t *frame_id) override;

  void Pin(frame_id_t frame_id) override;

  void Unpin(frame_id_t frame_id) override;

  size_t Size() override;

 private:
  size_t capacity_{0};
  size_t hand_{0};
  size_t size_{0};
  vector<bool> in_replacer_;
  vector<bool> ref_bit_;
  mutex latch_;
};

#endif  // MINISQL_CLOCK_REPLACER_H
