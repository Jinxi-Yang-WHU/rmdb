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
#include <climits>  
#include <cctype> 
#include <cstring>
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

inline bool is_valid_datetime(const std::string& s) {
    if (s.size() != 19) return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
    
    for (int i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (!isdigit(s[i])) return false;
    }
    
    int year = std::stoi(s.substr(0, 4));
    int month = std::stoi(s.substr(5, 2));
    int day = std::stoi(s.substr(8, 2));
    int hour = std::stoi(s.substr(11, 2));
    int minute = std::stoi(s.substr(14, 2));
    int second = std::stoi(s.substr(17, 2));
    
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (hour < 0 || hour > 23) return false;
    if (minute < 0 || minute > 59) return false;
    if (second < 0 || second > 59) return false;
    
    int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (is_leap) days_in_month[2] = 29;
    if (day < 1 || day > days_in_month[month]) return false;
    
    return true;
}

struct Value {
    ColType type;
    union {
        int int_val;
        float float_val;
        int64_t bigint_val;
    };
    std::string str_val;
    std::shared_ptr<RmRecord> raw;

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

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void cast_to(ColType target_type) {
        if (type == target_type) return;
        
        if (target_type == TYPE_DATETIME && type == TYPE_STRING) {
            if (!is_valid_datetime(str_val)) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
            }
            type = TYPE_DATETIME;
        } else if (target_type == TYPE_STRING && type == TYPE_DATETIME) {
            type = TYPE_STRING;
        } else if (target_type == TYPE_BIGINT && type == TYPE_INT) {
            set_bigint(static_cast<int64_t>(int_val));
        } else if (target_type == TYPE_INT && type == TYPE_BIGINT) {
            if (bigint_val > INT_MAX || bigint_val < INT_MIN) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
            }
            set_int(static_cast<int>(bigint_val));
        } else if (target_type == TYPE_FLOAT && type == TYPE_INT) {
            int int_val = this->int_val;
            set_float(static_cast<float>(int_val));
        } else if (target_type == TYPE_INT && type == TYPE_FLOAT) {
            float float_val = this->float_val;
            if (float_val != static_cast<int>(float_val)) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
            }
            set_int(static_cast<int>(float_val));
        } else if (target_type == TYPE_FLOAT && type == TYPE_BIGINT) {
            set_float(static_cast<float>(bigint_val));
        } else if (target_type == TYPE_BIGINT && type == TYPE_FLOAT) {
            float f = float_val;
            if (f > LLONG_MAX || f < LLONG_MIN || f != static_cast<int64_t>(f)) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
            }
            set_bigint(static_cast<int64_t>(f));
        } else {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
        }
    }

    void init_raw(int len) {
        if (raw != nullptr) {
            if (type == TYPE_INT) {
                assert(len == sizeof(int));
                *(int *)(raw->data) = int_val;
            } else if (type == TYPE_FLOAT) {
                assert(len == sizeof(float));
                *(float *)(raw->data) = float_val;
            } else if (type == TYPE_DATETIME) {
                if (len < (int)str_val.size()) {
                    throw StringOverflowError();
                }
                memset(raw->data, 0, len);
                memcpy(raw->data, str_val.c_str(), str_val.size());
            } else if (type == TYPE_STRING) {
                if (len < (int)str_val.size()) {
                    throw StringOverflowError();
                }
                memset(raw->data, 0, len);
                memcpy(raw->data, str_val.c_str(), str_val.size());
            } else if (type == TYPE_BIGINT) {
                assert(len == sizeof(int64_t));
                *(int64_t *)(raw->data) = bigint_val;
            }
            return;
        }
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
        // ==================== 新增以下 6 行 ====================
        else if (type == TYPE_DATETIME) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        } 
        // =====================================================
        else if (type == TYPE_BIGINT) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };
enum ArithOp { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV };

enum AggregateType {
    AGG_SUM,
    AGG_MAX,
    AGG_MIN,
    AGG_COUNT,
    AGG_COUNT_STAR
};

inline std::string get_default_agg_name(AggregateType type) {
    switch (type) {
        case AGG_SUM: return "sum";
        case AGG_MAX: return "max";
        case AGG_MIN: return "min";
        case AGG_COUNT:
        case AGG_COUNT_STAR: return "count";
    }
    return "";
}

struct AggregateInfo {
    AggregateType agg_type;
    TabCol col;        // 输入列，COUNT_STAR 时为空
    std::string alias; // 输出列名
    ColType out_type;  // 输出列类型
};

// Resolved SET clause used by analyzer/executor (after semantic analysis)
struct SetClause {
    TabCol lhs;              // target column
    Value rhs;               // constant right-hand side value
    bool is_rhs_expr = false; // true if rhs is col op const expression
    TabCol rhs_col;          // right-hand side column in expression
    ArithOp arith_op;        // ARITH_ADD or ARITH_SUB
    Value rhs_expr_val;      // constant part of expression

    SetClause() = default;
};

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

