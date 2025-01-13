/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

/**
 * @description: 申请行级共享锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID 记录所在的表的fd
 * @param {int} tab_fd
 */
bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::SHARED);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && request.lock_mode_ == LockMode::EXLUCSIVE) {
            if (request.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            return true;
        }
    }

    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    txn->get_lock_set()->insert(lock_data_id);
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 申请行级排他锁
 * @return {bool} 加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {Rid&} rid 加锁的目标记录ID
 * @param {int} tab_fd 记录所在的表的fd
 */
bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::EXLUCSIVE);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    bool has_self_lock = false;
    LockMode self_lock_mode = LockMode::SHARED;  // Default value
    bool has_conflict = false;

    // 1. Check our own locks
    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && request.txn_id_ == txn->get_transaction_id()) {
            has_self_lock = true;
            self_lock_mode = request.lock_mode_;
            if (self_lock_mode == LockMode::EXLUCSIVE) {
                return true;
            }
        }
    }

    // 2. check other transactions' locks
    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && request.txn_id_ != txn->get_transaction_id()) {
            has_conflict = true;
            break;
        }
    }

    // 3. Handle conflicts
    if (has_conflict) {
        if (has_self_lock) {
            request_queue.request_queue_.remove_if([txn_id = txn->get_transaction_id()](const LockRequest& req) {
                return req.txn_id_ == txn_id && req.granted_;
            });
            txn->get_lock_set()->erase(lock_data_id);
        }

        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
    }

    // 4. If we have an S lock, upgrade it to X
    if (has_self_lock) {
        request_queue.request_queue_.remove_if([txn_id = txn->get_transaction_id()](const LockRequest& req) {
            return req.txn_id_ == txn_id && req.granted_;
        });
    }

    // 5. Add and grant X lock
    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    // 6. Update transaction's lock set (only if we didn't have a lock before)
    if (!has_self_lock) {
        txn->get_lock_set()->insert(lock_data_id);
    }
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 申请表级读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::SHARED);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ &&
            (request.lock_mode_ == LockMode::EXLUCSIVE || request.lock_mode_ == LockMode::INTENTION_EXCLUSIVE ||
             request.lock_mode_ == LockMode::S_IX)) {
            if (request.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            return true;
        }
    }

    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    txn->get_lock_set()->insert(lock_data_id);
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 申请表级写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::EXLUCSIVE);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && request.txn_id_ != txn->get_transaction_id()) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    txn->get_lock_set()->insert(lock_data_id);
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 申请表级意向读锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::INTENTION_SHARED);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && request.lock_mode_ == LockMode::EXLUCSIVE) {
            if (request.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            return true;
        }
    }

    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    txn->get_lock_set()->insert(lock_data_id);
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 申请表级意向写锁
 * @return {bool} 返回加锁是否成功
 * @param {Transaction*} txn 要申请锁的事务对象指针
 * @param {int} tab_fd 目标表的fd
 */
bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    LockRequest lock_request(txn->get_transaction_id(), LockMode::INTENTION_EXCLUSIVE);

    std::scoped_lock lock(latch_);

    auto& request_queue = lock_table_[lock_data_id];

    for (const auto& request : request_queue.request_queue_) {
        if (request.granted_ && (request.lock_mode_ == LockMode::SHARED || request.lock_mode_ == LockMode::EXLUCSIVE)) {
            if (request.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
            return true;
        }
    }

    request_queue.request_queue_.push_back(lock_request);
    auto& added_request = request_queue.request_queue_.back();
    added_request.granted_ = true;

    txn->get_lock_set()->insert(lock_data_id);
    if (txn->get_state() == TransactionState::DEFAULT) {
        txn->set_state(TransactionState::GROWING);
    }

    return true;
}

/**
 * @description: 释放锁
 * @return {bool} 返回解锁是否成功
 * @param {Transaction*} txn 要释放锁的事务对象指针
 * @param {LockDataId} lock_data_id 要释放的锁ID
 */
bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    std::scoped_lock lock(latch_);

    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) {
        return false;
    }

    auto& request_queue = it->second;

    for (auto req_it = request_queue.request_queue_.begin(); req_it != request_queue.request_queue_.end();) {
        if (req_it->txn_id_ == txn->get_transaction_id() && req_it->granted_) {
            // Remove the lock request
            req_it = request_queue.request_queue_.erase(req_it);

            // Update transaction state to SHRINKING if it was in GROWING
            if (txn->get_state() == TransactionState::GROWING) {
                txn->set_state(TransactionState::SHRINKING);
            }

            // If queue is empty after removal, remove it from lock table
            if (request_queue.request_queue_.empty()) {
                lock_table_.erase(it);
            }

            return true;
        } else {
            ++req_it;
        }
    }

    return false;
}

bool LockManager::lock_gap(Transaction* txn, const std::string& left_key, const std::string& right_key, int table_id) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lock(gap_latch_);

    for (const auto& entry : gap_lock_table_) {
        const auto& existing_range = entry.first;
        const auto& existing_queue = entry.second;

        if (existing_range.table_id != table_id) {
            continue;
        }
        bool has_overlap = false;

        if (left_key.size() == existing_range.left_key.size() && right_key.size() == existing_range.right_key.size()) {
            has_overlap = !(memcmp(right_key.data(), existing_range.left_key.data(), right_key.size()) <= 0 ||
                            memcmp(left_key.data(), existing_range.right_key.data(), left_key.size()) >= 0);
        }

        if (has_overlap) {
            for (const auto& req : existing_queue.request_queue) {
                if (req.granted && req.txn_id != txn->get_transaction_id()) {
                    throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                }
            }
        }
    }

    GapLockRange range{left_key, right_key, table_id};
    auto& lock_queue = gap_lock_table_[range];
    lock_queue.request_queue.push_back(GapLockRequest(txn->get_transaction_id()));
    lock_queue.request_queue.back().granted = true;

    txn->get_gap_lock_set()->insert(GapLockId(left_key, right_key, table_id));

    return true;
}

bool LockManager::unlock_gap(Transaction* txn, const std::string& left_key, const std::string& right_key,
                             int table_id) {
    GapLockRange range{left_key, right_key, table_id};

    std::unique_lock<std::mutex> lock(gap_latch_);

    auto it = gap_lock_table_.find(range);
    if (it == gap_lock_table_.end()) {
        return true;
    }

    auto& lock_queue = it->second;

    lock_queue.request_queue.remove_if(
        [txn_id = txn->get_transaction_id()](const GapLockRequest& req) { return req.txn_id == txn_id; });

    txn->get_gap_lock_set()->erase(GapLockId(left_key, right_key, table_id));

    return true;
}