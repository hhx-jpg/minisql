#include "recovery/recovery_manager.h"

namespace {

void UndoDmlLog(const LogRecPtr &log, KvDatabase &data) {
  switch (log->type_) {
    case LogRecType::kInsert:
      data.erase(log->ins_key_);
      break;
    case LogRecType::kDelete:
      data[log->del_key_] = log->del_val_;
      break;
    case LogRecType::kUpdate:
      data[log->old_key_] = log->old_val_;
      if (log->old_key_ != log->new_key_) {
        data.erase(log->new_key_);
      }
      break;
    default:
      break;
  }
}

}  // namespace

void RecoveryManager::Init(CheckPoint &last_checkpoint) {
  persist_lsn_ = last_checkpoint.checkpoint_lsn_;
  active_txns_ = last_checkpoint.active_txns_;
  data_ = last_checkpoint.persist_data_;
}

void RecoveryManager::RedoPhase() {
  for (auto &[lsn, log] : log_recs_) {
    if (lsn <= persist_lsn_) continue;

    switch (log->type_) {
      case LogRecType::kBegin: {
        active_txns_[log->txn_id_] = lsn;
        break;
      }
      case LogRecType::kInsert: {
        data_[log->ins_key_] = log->ins_val_;
        active_txns_[log->txn_id_] = lsn;
        break;
      }
      case LogRecType::kDelete: {
        data_.erase(log->del_key_);
        active_txns_[log->txn_id_] = lsn;
        break;
      }
      case LogRecType::kUpdate: {
        data_[log->new_key_] = log->new_val_;
        if (log->old_key_ != log->new_key_) {
          data_.erase(log->old_key_);
        }
        active_txns_[log->txn_id_] = lsn;
        break;
      }
      case LogRecType::kCommit: {
        active_txns_.erase(log->txn_id_);
        break;
      }
      case LogRecType::kAbort: {
        // Roll back all operations of the aborted transaction
        for (auto rit = log_recs_.rbegin(); rit != log_recs_.rend(); ++rit) {
          if (rit->first > lsn) continue;
          auto &undo_log = rit->second;
          if (undo_log->txn_id_ != log->txn_id_) continue;
          if (undo_log->type_ == LogRecType::kBegin) break;
          UndoDmlLog(undo_log, data_);
        }
        active_txns_.erase(log->txn_id_);
        break;
      }
      default:
        break;
    }
  }
}

void RecoveryManager::UndoPhase() {
  for (auto rit = log_recs_.rbegin(); rit != log_recs_.rend(); ++rit) {
    auto &log = rit->second;
    if (active_txns_.find(log->txn_id_) == active_txns_.end()) continue;
    if (log->type_ == LogRecType::kBegin) {
      active_txns_.erase(log->txn_id_);
      continue;
    }
    UndoDmlLog(log, data_);
  }
}
