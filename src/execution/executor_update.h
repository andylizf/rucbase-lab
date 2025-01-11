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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (rids_.empty()) {
            return nullptr;
        }

        std::vector<Rid> rids_to_update;
        std::vector<std::unique_ptr<RmRecord>> old_recs;
        std::vector<std::unique_ptr<RmRecord>> new_recs;

        // First collect all records that need to be updated
        for (const auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            auto new_rec = std::make_unique<RmRecord>(*old_rec);

            if (!evaluate_conditions(conds_, tab_.cols, old_rec.get())) {
                continue;
            }

            // Update values according to set_clauses
            for (auto &set_clause : set_clauses_) {
                TabCol search_col = set_clause.lhs;
                if (search_col.tab_name.empty()) {
                    // Default to the current table name
                    search_col.tab_name = tab_name_;
                }
                auto col = get_col(tab_.cols, search_col);
                memcpy(new_rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }

            rids_to_update.push_back(rid);
            old_recs.push_back(std::make_unique<RmRecord>(*old_rec));
            new_recs.push_back(std::move(new_rec));
        }

        if (rids_to_update.empty()) {
            return nullptr;
        }

        // Check all indexes for duplicates for all record to maintain integrity constraint
        for (size_t i = 0; i < tab_.cols.size(); i++) {
            auto &col = tab_.cols[i];
            if (col.index) {
                // Get index handle for this column
                std::vector<std::string> index_cols = {col.name};
                auto ih =
                    sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_cols)).get();

                // For each record to be updated, check if its new value would violate uniqueness
                for (size_t j = 0; j < new_recs.size(); j++) {
                    std::vector<Rid> rids;
                    ih->get_value(new_recs[j]->data + col.offset, &rids, context_->txn_);
                    for (const auto &r : rids) {
                        bool is_self = false;
                        for (const auto &rid : rids_to_update) {
                            if (r.page_no == rid.page_no && r.slot_no == rid.slot_no) {
                                is_self = true;
                                break;
                            }
                        }
                        if (!is_self) {
                            throw InternalError("Duplicate key found in index for column: " + col.name);
                        }
                    }
                }
            }
        }

        // Then update all indexes and records
        for (size_t j = 0; j < rids_to_update.size(); j++) {
            // Add to write set before actual update
            if (context_->txn_ != nullptr) {
                WriteRecord *write_record =
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rids_to_update[j], *old_recs[j].get());
                context_->txn_->append_write_record(write_record);
            }

            // Update indexes
            for (size_t i = 0; i < tab_.cols.size(); i++) {
                auto &col = tab_.cols[i];
                if (col.index) {
                    std::vector<std::string> index_cols = {col.name};
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_cols))
                                  .get();

                    ih->delete_entry(old_recs[j]->data + col.offset, context_->txn_);
                    ih->insert_entry(new_recs[j]->data + col.offset, rids_to_update[j], context_->txn_);
                }
            }

            // Update record in table file
            fh_->update_record(rids_to_update[j], new_recs[j]->data, context_);
        }

        // Return the first updated record
        if (!new_recs.empty()) {
            return std::move(new_recs[0]);
        }

        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};