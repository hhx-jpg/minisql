#include "recovery/log_rec.h"

std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_ = {};
lsn_t LogRec::next_lsn_ = 0;

LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kInsert;
  rec->txn_id_ = txn_id;
  rec->ins_key_ = std::move(ins_key);
  rec->ins_val_ = ins_val;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kDelete;
  rec->txn_id_ = txn_id;
  rec->del_key_ = std::move(del_key);
  rec->del_val_ = del_val;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kUpdate;
  rec->txn_id_ = txn_id;
  rec->old_key_ = std::move(old_key);
  rec->old_val_ = old_val;
  rec->new_key_ = std::move(new_key);
  rec->new_val_ = new_val;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

LogRecPtr CreateBeginLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kBegin;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

LogRecPtr CreateCommitLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kCommit;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}

LogRecPtr CreateAbortLog(txn_id_t txn_id) {
  auto rec = std::make_shared<LogRec>();
  rec->type_ = LogRecType::kAbort;
  rec->txn_id_ = txn_id;
  rec->lsn_ = LogRec::next_lsn_++;
  auto it = LogRec::prev_lsn_map_.find(txn_id);
  rec->prev_lsn_ = (it != LogRec::prev_lsn_map_.end()) ? it->second : INVALID_LSN;
  LogRec::prev_lsn_map_[txn_id] = rec->lsn_;
  return rec;
}
