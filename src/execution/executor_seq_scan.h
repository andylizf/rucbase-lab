/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager *sm_manager_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void beginTuple() override {
        scan_ = std::make_unique<RmScan>(fh_);

        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        if (!tab.indexes.empty()) {
            auto &index = tab.indexes[0];
            std::string min_key(index.col_tot_len, 0);
            std::string max_key(index.col_tot_len, 0);
            memset(const_cast<char *>(max_key.data()), 0xFF, index.col_tot_len);

            bool has_range = false;
            for (const auto &cond : fed_conds_) {
                if (cond.lhs_col.col_name == index.cols[0].name) {
                    char *key_data = new char[index.col_tot_len];
                    memset(key_data, 0, index.col_tot_len);
                    memcpy(key_data, cond.rhs_val.raw->data, sizeof(int));

                    if (cond.op == OP_GT || cond.op == OP_GE) {
                        min_key = std::string(key_data, index.col_tot_len);
                        has_range = true;
                    } else if (cond.op == OP_LT || cond.op == OP_LE) {
                        max_key = std::string(key_data, index.col_tot_len);
                        has_range = true;
                    }
                    delete[] key_data;
                }
            }

            if (!has_range) {
                memset(const_cast<char *>(max_key.data()), 0xFF, index.col_tot_len);
            }

            bool lock_result = context_->lock_mgr_->lock_gap(context_->txn_, min_key, max_key, fh_->GetFd());
            if (!lock_result) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
            }
        }

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);

            if (evaluate_conditions(fed_conds_, cols_, rec.get())) {
                return;
            }

            scan_->next();
        }
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void nextTuple() override {
        scan_->next();

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);

            if (evaluate_conditions(fed_conds_, cols_, rec.get())) {
                return;
            }

            scan_->next();
        }
    }

    bool is_end() const override { return scan_->is_end(); }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> Next() override {
        assert(!scan_->is_end());
        return std::make_unique<RmRecord>(*fh_->get_record(rid_, context_));
    }

    Rid &rid() override { return rid_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }
};