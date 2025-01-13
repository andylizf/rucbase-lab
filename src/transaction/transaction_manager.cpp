/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction*> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction* TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn != nullptr) {
        return txn;
    }

    txn_id_t txn_id = next_txn_id_++;
    timestamp_t start_ts = next_timestamp_++;

    txn = new Transaction(txn_id);
    txn->set_start_ts(start_ts);
    txn->set_state(TransactionState::GROWING);

    txn_map[txn_id] = txn;

    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // 释放所有gap locks
    auto gap_lock_set = txn->get_gap_lock_set();
    for (const auto& gap_lock : *gap_lock_set) {
        lock_manager_->unlock_gap(txn, gap_lock.left_key, gap_lock.right_key, gap_lock.table_id);
    }
    gap_lock_set->clear();

    // 释放所有record locks
    auto lock_set = txn->get_lock_set();
    for (auto lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }

    lock_set->clear();
    txn->get_write_set()->clear();
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();

    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction* txn, LogManager* log_manager) {
    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        auto write_record = write_set->back();
        auto table_name = write_record->GetTableName();
        auto rid = write_record->GetRid();
        auto fh = sm_manager_->fhs_.at(table_name).get();

        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE: {
                fh->delete_record(rid, nullptr);
                break;
            }
            case WType::DELETE_TUPLE: {
                RmRecord record(write_record->GetRecord());
                fh->insert_record(record.data, nullptr);
                break;
            }
            case WType::UPDATE_TUPLE: {
                RmRecord record(write_record->GetRecord());
                fh->update_record(rid, record.data, nullptr);
                break;
            }
        }
        write_set->pop_back();
        delete write_record;
    }

    // 释放所有gap locks
    auto gap_lock_set = txn->get_gap_lock_set();
    for (const auto& gap_lock : *gap_lock_set) {
        lock_manager_->unlock_gap(txn, gap_lock.left_key, gap_lock.right_key, gap_lock.table_id);
    }
    gap_lock_set->clear();

    // 释放所有record locks
    auto lock_set = txn->get_lock_set();
    for (auto lock_id : *lock_set) {
        lock_manager_->unlock(txn, lock_id);
    }

    lock_set->clear();
    write_set->clear();
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();

    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::ABORTED);
}