#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <unordered_map>
#include <utility>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * TODO: Student Implement
 */
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};
    txn_id_t txn_id_{0};

    // for insert / delete
    KeyType ins_key_{};
    ValType ins_val_{};
    KeyType del_key_{};
    ValType del_val_{};

    // for update
    KeyType old_key_{};
    ValType old_val_{};
    KeyType new_key_{};
    ValType new_val_{};

    /* used for testing only */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

typedef std::shared_ptr<LogRec> LogRecPtr;

LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val);

LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val);

LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val);

LogRecPtr CreateBeginLog(txn_id_t txn_id);

LogRecPtr CreateCommitLog(txn_id_t txn_id);

LogRecPtr CreateAbortLog(txn_id_t txn_id);

#endif  // MINISQL_LOG_REC_H
