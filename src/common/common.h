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

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "defs.h"
#include "record/rm_defs.h"

struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

inline bool evaluate_compare(const char *lhs, ColType lhs_type, const char *rhs, ColType rhs_type, CompOp op,
                             size_t len = sizeof(int)) {
    switch (lhs_type) {
        case TYPE_INT: {
            int lhs_val = *(int *)lhs;
            int rhs_val = *(int *)rhs;
            switch (op) {
                case OP_EQ:
                    return lhs_val == rhs_val;
                case OP_NE:
                    return lhs_val != rhs_val;
                case OP_LT:
                    return lhs_val < rhs_val;
                case OP_GT:
                    return lhs_val > rhs_val;
                case OP_LE:
                    return lhs_val <= rhs_val;
                case OP_GE:
                    return lhs_val >= rhs_val;
            }
        }
        case TYPE_FLOAT: {
            float lhs_val = *(float *)lhs;
            float rhs_val = *(float *)rhs;
            switch (op) {
                case OP_EQ:
                    return lhs_val == rhs_val;
                case OP_NE:
                    return lhs_val != rhs_val;
                case OP_LT:
                    return lhs_val < rhs_val;
                case OP_GT:
                    return lhs_val > rhs_val;
                case OP_LE:
                    return lhs_val <= rhs_val;
                case OP_GE:
                    return lhs_val >= rhs_val;
            }
        }
        case TYPE_STRING: {
            int cmp = memcmp(lhs, rhs, len);

            switch (op) {
                case OP_EQ:
                    return cmp == 0;
                case OP_NE:
                    return cmp != 0;
                case OP_LT:
                    return cmp < 0;
                case OP_GT:
                    return cmp > 0;
                case OP_LE:
                    return cmp <= 0;
                case OP_GE:
                    return cmp >= 0;
            }
        }
    }
    assert(false);
}

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};