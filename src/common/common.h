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

    void cast_to(ColType target_type) {
        if (type == target_type) return;
        if (target_type == TYPE_FLOAT && type == TYPE_INT) {
            int int_val = this->int_val;
            set_float(static_cast<float>(int_val));
        } else if (target_type == TYPE_INT && type == TYPE_FLOAT) {
            float float_val = this->float_val;
            if (float_val != static_cast<int>(float_val)) {
                throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
            }
            set_int(static_cast<int>(float_val));
        } else {
            throw IncompatibleTypeError(coltype2str(target_type), coltype2str(type));
        }
    }

    void init_raw(int len) {
        if (raw != nullptr) {
            // 已初始化过：重新填充数据（支持类型转换后再次调用）
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
            return;
        }
        // 首次初始化
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
enum ArithOp { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV };

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
    bool is_rhs_expr = false;   // 新增：右侧是否为表达式
    TabCol rhs_col;             // 新增：表达式引用的列（如 score）
    ArithOp arith_op;           // 新增：算术运算符
    Value rhs_expr_val;         // 新增：表达式中的常量值（如 5）
};