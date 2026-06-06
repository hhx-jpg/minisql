#include "concurrency/lock_manager.h"

#include <algorithm>
#include <iostream>
#include <thread>

#include "common/rowid.h"
#include "concurrency/txn.h"
#include "concurrency/txn_manager.h"

void LockManager::SetTxnMgr(TxnManager *txn_mgr) { txn_mgr_ = txn_mgr; }

bool LockManager::LockShared(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);
    LockPrepare(txn, rid);

    auto &req_queue = lock_table_[rid];
    if (txn->GetExclusiveLockSet().count(rid) != 0 || txn->GetSharedLockSet().count(rid) != 0) {
        return true;
    }

    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kShared);
    auto request = req_queue.GetLockRequestIter(txn->GetTxnId());

    while (req_queue.is_writing_ || req_queue.is_upgrading_) {
        req_queue.cv_.wait(lock);
        CheckAbort(txn, req_queue);
        request = req_queue.GetLockRequestIter(txn->GetTxnId());
    }

    request->granted_ = LockMode::kShared;
    req_queue.sharing_cnt_++;
    txn->GetSharedLockSet().emplace(rid);
    return true;
}

bool LockManager::LockExclusive(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);
    LockPrepare(txn, rid);

    auto &req_queue = lock_table_[rid];
    if (txn->GetExclusiveLockSet().count(rid) != 0) {
        return true;
    }
    if (txn->GetSharedLockSet().count(rid) != 0) {
        lock.unlock();
        return LockUpgrade(txn, rid);
    }

    req_queue.EmplaceLockRequest(txn->GetTxnId(), LockMode::kExclusive);
    auto request = req_queue.GetLockRequestIter(txn->GetTxnId());

    while (req_queue.is_writing_ || req_queue.sharing_cnt_ > 0) {
        req_queue.cv_.wait(lock);
        CheckAbort(txn, req_queue);
        request = req_queue.GetLockRequestIter(txn->GetTxnId());
    }

    request->granted_ = LockMode::kExclusive;
    req_queue.is_writing_ = true;
    txn->GetExclusiveLockSet().emplace(rid);
    return true;
}

bool LockManager::LockUpgrade(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);
    LockPrepare(txn, rid);

    auto &req_queue = lock_table_[rid];
    if (txn->GetExclusiveLockSet().count(rid) != 0) {
        return true;
    }
    if (txn->GetSharedLockSet().count(rid) == 0) {
        lock.unlock();
        return LockExclusive(txn, rid);
    }
    if (req_queue.is_upgrading_) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kUpgradeConflict);
    }

    req_queue.is_upgrading_ = true;
    auto request = req_queue.GetLockRequestIter(txn->GetTxnId());
    request->lock_mode_ = LockMode::kExclusive;

    while (req_queue.is_writing_ || req_queue.sharing_cnt_ > 1) {
        req_queue.cv_.wait(lock);
        if (txn->GetState() == TxnState::kAborted) {
            req_queue.is_upgrading_ = false;
            req_queue.cv_.notify_all();
            throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
        }
        request = req_queue.GetLockRequestIter(txn->GetTxnId());
    }

    request->granted_ = LockMode::kExclusive;
    req_queue.is_writing_ = true;
    req_queue.sharing_cnt_--;
    req_queue.is_upgrading_ = false;
    txn->GetSharedLockSet().erase(rid);
    txn->GetExclusiveLockSet().emplace(rid);
    req_queue.cv_.notify_all();
    return true;
}

bool LockManager::Unlock(Txn *txn, const RowId &rid) {
    std::unique_lock<std::mutex> lock(latch_);
    auto table_iter = lock_table_.find(rid);
    if (table_iter == lock_table_.end()) {
        return false;
    }
    auto &req_queue = table_iter->second;
    auto req_iter_map_iter = req_queue.req_list_iter_map_.find(txn->GetTxnId());
    if (req_iter_map_iter == req_queue.req_list_iter_map_.end()) {
        return false;
    }

    auto request = req_iter_map_iter->second;
    auto granted = request->granted_;
    if (granted == LockMode::kShared) {
        req_queue.sharing_cnt_--;
        txn->GetSharedLockSet().erase(rid);
    } else if (granted == LockMode::kExclusive) {
        req_queue.is_writing_ = false;
        txn->GetExclusiveLockSet().erase(rid);
    }

    if (txn->GetState() == TxnState::kGrowing) {
        if (txn->GetIsolationLevel() == IsolationLevel::kRepeatedRead ||
            (txn->GetIsolationLevel() != IsolationLevel::kRepeatedRead && granted == LockMode::kExclusive)) {
            txn->SetState(TxnState::kShrinking);
        }
    }

    req_queue.EraseLockRequest(txn->GetTxnId());
    req_queue.cv_.notify_all();
    if (req_queue.req_list_.empty()) {
        lock_table_.erase(table_iter);
    }
    return true;
}

void LockManager::LockPrepare(Txn *txn, const RowId &rid) {
    if (txn->GetState() == TxnState::kAborted) {
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
    }
    if (txn->GetIsolationLevel() == IsolationLevel::kReadUncommitted) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockSharedOnReadUncommitted);
    }
    if (txn->GetState() == TxnState::kShrinking) {
        txn->SetState(TxnState::kAborted);
        throw TxnAbortException(txn->GetTxnId(), AbortReason::kLockOnShrinking);
    }
    lock_table_.try_emplace(rid);
}

void LockManager::CheckAbort(Txn *txn, LockManager::LockRequestQueue &req_queue) {
    if (txn->GetState() != TxnState::kAborted) {
        return;
    }
    req_queue.EraseLockRequest(txn->GetTxnId());
    req_queue.cv_.notify_all();
    throw TxnAbortException(txn->GetTxnId(), AbortReason::kDeadlock);
}

void LockManager::AddEdge(txn_id_t t1, txn_id_t t2) { waits_for_[t1].emplace(t2); }

void LockManager::RemoveEdge(txn_id_t t1, txn_id_t t2) {
    auto iter = waits_for_.find(t1);
    if (iter == waits_for_.end()) {
        return;
    }
    iter->second.erase(t2);
    if (iter->second.empty()) {
        waits_for_.erase(iter);
    }
}

bool LockManager::HasCycle(txn_id_t &newest_tid_in_cycle) {
    std::set<txn_id_t> nodes;
    for (const auto &entry : waits_for_) {
        nodes.emplace(entry.first);
        for (auto dst : entry.second) {
            nodes.emplace(dst);
        }
    }

    std::unordered_set<txn_id_t> visited;
    std::unordered_set<txn_id_t> active;
    std::vector<txn_id_t> path;

    std::function<bool(txn_id_t)> dfs = [&](txn_id_t node) {
        visited.emplace(node);
        active.emplace(node);
        path.emplace_back(node);

        auto iter = waits_for_.find(node);
        if (iter != waits_for_.end()) {
            for (auto next : iter->second) {
                if (active.count(next) != 0) {
                    newest_tid_in_cycle = next;
                    for (auto path_iter = path.rbegin(); path_iter != path.rend() && *path_iter != next; ++path_iter) {
                        newest_tid_in_cycle = std::max(newest_tid_in_cycle, *path_iter);
                    }
                    return true;
                }
                if (visited.count(next) == 0 && dfs(next)) {
                    return true;
                }
            }
        }

        path.pop_back();
        active.erase(node);
        return false;
    };

    newest_tid_in_cycle = INVALID_TXN_ID;
    for (auto node : nodes) {
        if (visited.count(node) == 0 && dfs(node)) {
            return true;
        }
    }
    return false;
}

void LockManager::DeleteNode(txn_id_t txn_id) {
    waits_for_.erase(txn_id);

    auto *txn = txn_mgr_->GetTransaction(txn_id);

    for (const auto &row_id: txn->GetSharedLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }

    for (const auto &row_id: txn->GetExclusiveLockSet()) {
        for (const auto &lock_req: lock_table_[row_id].req_list_) {
            if (lock_req.granted_ == LockMode::kNone) {
                RemoveEdge(lock_req.txn_id_, txn_id);
            }
        }
    }
}

void LockManager::RunCycleDetection() {
    while (enable_cycle_detection_) {
        std::this_thread::sleep_for(cycle_detection_interval_);
        std::unique_lock<std::mutex> lock(latch_);
        waits_for_.clear();

        for (auto &table_entry : lock_table_) {
            auto &req_queue = table_entry.second;
            for (const auto &waiting_req : req_queue.req_list_) {
                if (waiting_req.granted_ == waiting_req.lock_mode_) {
                    continue;
                }
                auto *waiting_txn = txn_mgr_->GetTransaction(waiting_req.txn_id_);
                if (waiting_txn == nullptr || waiting_txn->GetState() == TxnState::kAborted) {
                    continue;
                }
                for (const auto &granted_req : req_queue.req_list_) {
                    if (granted_req.granted_ == LockMode::kNone || granted_req.txn_id_ == waiting_req.txn_id_) {
                        continue;
                    }
                    auto *granted_txn = txn_mgr_->GetTransaction(granted_req.txn_id_);
                    if (granted_txn == nullptr || granted_txn->GetState() == TxnState::kAborted) {
                        continue;
                    }
                    AddEdge(waiting_req.txn_id_, granted_req.txn_id_);
                }
            }
        }

        txn_id_t txn_id = INVALID_TXN_ID;
        while (HasCycle(txn_id)) {
            auto *txn = txn_mgr_->GetTransaction(txn_id);
            if (txn != nullptr) {
                txn->SetState(TxnState::kAborted);
            }
            DeleteNode(txn_id);
            txn_id = INVALID_TXN_ID;
        }

        for (auto &table_entry : lock_table_) {
            table_entry.second.cv_.notify_all();
        }
    }
}

std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetEdgeList() {
    std::vector<std::pair<txn_id_t, txn_id_t>> result;
    for (const auto &entry : waits_for_) {
        for (auto dst : entry.second) {
            result.emplace_back(entry.first, dst);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
