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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;   // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;  // 右儿子节点（需要join的表）
    size_t len_;                               // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                // join后获得的记录的字段
    std::vector<Condition> fed_conds_;         // join条件
    bool isend;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }

        right_->beginTuple();
        if (right_->is_end()) {
            isend = true;
            return;
        }

        auto left_record = left_->Next();
        auto right_record = right_->Next();
        if (left_record == nullptr || right_record == nullptr ||
            !evaluate_join_conditions(left_record.get(), right_record.get())) {
            nextTuple();
        }
    }

    void nextTuple() override {
        if (isend) return;

        while (true) {
            // Try next right record
            right_->nextTuple();
            if (!right_->is_end()) {
                auto left_record = left_->Next();
                auto right_record = right_->Next();
                if (left_record != nullptr && right_record != nullptr &&
                    evaluate_join_conditions(left_record.get(), right_record.get())) {
                    return;  // Found next matching record
                }
                continue;
            }

            // Right table exhausted, move to next left record
            left_->nextTuple();
            if (left_->is_end()) {
                isend = true;
                return;
            }

            // Reset right table
            right_->beginTuple();
            auto left_record = left_->Next();
            auto right_record = right_->Next();
            if (left_record != nullptr && right_record != nullptr &&
                evaluate_join_conditions(left_record.get(), right_record.get())) {
                return;
            }
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) {
            return nullptr;
        }

        auto left_record = left_->Next();
        auto right_record = right_->Next();

        if (left_record == nullptr || right_record == nullptr) {
            return nullptr;
        }

        auto merged_record = std::make_unique<RmRecord>(len_);
        memcpy(merged_record->data, left_record->data, left_->tupleLen());
        memcpy(merged_record->data + left_->tupleLen(), right_record->data, right_->tupleLen());
        return merged_record;
    }

    bool evaluate_join_conditions(RmRecord *left_record, RmRecord *right_record) {
        return evaluate_conditions(fed_conds_, left_->cols(), left_record, right_->cols(), right_record);
    }

    bool is_end() const override { return isend; }

    Rid &rid() override { return _abstract_rid; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }
};