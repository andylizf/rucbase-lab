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

#include "defs.h"
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表名称
    TabMeta tab_;                       // 表的元数据
    std::vector<Condition> conds_;      // 扫描条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // 需要读取的字段
    size_t len_;                        // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;  // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    bool satisfy_conditions(const RmRecord *rec) { return evaluate_conditions(fed_conds_, cols_, rec); }

    // Helper function to build index key from conditions
    std::pair<std::unique_ptr<char[]>, std::unique_ptr<char[]>> build_key_range_from_conditions(
        const std::vector<Condition> &conds) {
        auto lower_key = std::make_unique<char[]>(index_meta_.col_tot_len);
        auto upper_key = std::make_unique<char[]>(index_meta_.col_tot_len);
        int offset = 0;

        for (size_t i = 0; i < index_meta_.col_num; i++) {
            const auto &index_col = index_meta_.cols[i];
            bool found_eq = false;
            bool found_lower = false;
            bool found_upper = false;

            memset(lower_key.get() + offset, 0, index_col.len);
            memset(upper_key.get() + offset, 0xFF, index_col.len);

            for (const auto &cond : conds) {
                if (cond.lhs_col.col_name == index_col.name) {
                    if (cond.op == OP_EQ) {
                        memcpy(lower_key.get() + offset, cond.rhs_val.raw->data, index_col.len);
                        memcpy(upper_key.get() + offset, cond.rhs_val.raw->data, index_col.len);
                        found_eq = true;
                        break;
                    } else if (cond.op == OP_LT || cond.op == OP_LE) {
                        memcpy(upper_key.get() + offset, cond.rhs_val.raw->data, index_col.len);
                        found_upper = true;
                    } else if (cond.op == OP_GT || cond.op == OP_GE) {
                        memcpy(lower_key.get() + offset, cond.rhs_val.raw->data, index_col.len);
                        found_lower = true;
                    }
                }
            }

            if (!found_eq && !found_lower && !found_upper && i > 0) {
                break;
            }

            offset += index_col.len;
        }

        return {std::move(lower_key), std::move(upper_key)};
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                      std::vector<std::string> index_col_names, Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        auto [lower_key, upper_key] = build_key_range_from_conditions(fed_conds_);
        auto ih_ =
            sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_)).get();

        if (lower_key != nullptr && upper_key != nullptr) {
            Iid lower = ih_->lower_bound(lower_key.get());
            Iid upper = ih_->upper_bound(upper_key.get());

            scan_ = std::make_unique<IxScan>(ih_, lower, upper, sm_manager_->get_bpm());

            std::string lower_key_str(lower_key.get(), index_meta_.col_tot_len);
            std::string upper_key_str(upper_key.get(), index_meta_.col_tot_len);

            bool lock_result =
                context_->lock_mgr_->lock_gap(context_->txn_, lower_key_str, upper_key_str, fh_->GetFd());
            if (!lock_result) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
            }
        } else {
            scan_ = std::make_unique<IxScan>(ih_, ih_->leaf_begin(), ih_->leaf_end(), sm_manager_->get_bpm());

            std::string min_key(index_meta_.col_tot_len, 0);
            std::string max_key(index_meta_.col_tot_len, 0);
            memset(const_cast<char *>(max_key.data()), 0xFF, index_meta_.col_tot_len);

            bool lock_result = context_->lock_mgr_->lock_gap(context_->txn_, min_key, max_key, fh_->GetFd());
            if (!lock_result) {
                throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
            }
        }

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (satisfy_conditions(rec.get())) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_->is_end()) {
            return;
        }

        scan_->next();

        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (satisfy_conditions(rec.get())) {
                return;
            }
            scan_->next();
        }
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        assert(!is_end());
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }
};