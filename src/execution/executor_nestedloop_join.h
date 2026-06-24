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

/*
beginTuple():  左表.beginTuple() → 右表.beginTuple() → 找第一个满足条件的匹配
nextTuple():   右表.nextTuple() → 找下一个满足条件的匹配（右表耗尽则换左表下一行）
Next():        把当前 left_rec_ 和 right_rec_ 拼接成 RmRecord 返回
is_end():      左表是否耗尽
*/

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    std::unique_ptr<RmRecord> left_rec_;   // 当前左表记录
    std::unique_ptr<RmRecord> right_rec_;  // 当前右表记录

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        // 拼接后长度 = 左表长度 + 右表长度
        len_ = left_->tupleLen() + right_->tupleLen();
        // 左表字段直接拷贝
        cols_ = left_->cols();

        // 右表字段拷贝，但每个字段的 offset 要加上左表长度
        // 这样右表字段在拼接记录中就不会和左表重叠
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        // std::cerr << "[JOIN DEBUG] left=" << left_->cols()[0].tab_name << " right=" << right_->cols()[0].tab_name << std::endl;

    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        // 1. 启动左表遍历器，定位到第一条有效记录
        left_->beginTuple();
        
        // 2. 如果左表为空，直接结束
        if (left_->is_end()) {
            isend = true;
            return;
        }
        
        // 3. 启动右表遍历器
        right_->beginTuple();
        
        // 4. 在嵌套循环中找到第一个满足连接条件的匹配对
        find_next_valid();
    }

    void nextTuple() override {
        // 只让右表步进，左表不动
        // 然后在新的 (left, right) 组合中找下一个有效匹配
        right_->nextTuple();
        find_next_valid();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (isend) return nullptr;
        
        // 1. 分配拼接后的定长记录
        auto rec = std::make_unique<RmRecord>(len_);
        
        // 2. 拷贝左表数据到 [0, left_len)
        memcpy(rec->data, left_rec_->data, left_->tupleLen());
        
        // 3. 拷贝右表数据到 [left_len, len_)
        memcpy(rec->data + left_->tupleLen(), right_rec_->data, right_->tupleLen());
        
        return rec;
    }

    bool is_end() const override { return isend; }

    Rid &rid() override { return _abstract_rid; }

   private:
    void find_next_valid() {
        // 外层循环：左表逐行
        while (!left_->is_end()) {
            // 内层循环：右表逐行
            while (!right_->is_end()) {
                // 读取当前左右记录（不移动游标）
                left_rec_ = left_->Next();
                right_rec_ = right_->Next();
                
                // 如果无连接条件，或条件满足，则找到有效匹配
                if (eval_join_conds()) {
                    isend = false;
                    return;
                }
                
                // 不满足，右表下移
                right_->nextTuple();
            }
            
            // 右表耗尽，左表下移一行
            left_->nextTuple();
            
            // 如果左表还有数据，重置右表到开头，继续匹配
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
        
        // 左表也耗尽了，没有更多匹配
        isend = true;
    }

    bool eval_join_conds() {
        for (auto &cond : fed_conds_) {
            auto lhs_it = get_col(cols_, cond.lhs_col);
            char *lhs_data = get_col_data(lhs_it->offset);
            char *rhs_data = nullptr;
            
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    cond.rhs_val.init_raw(lhs_it->len);
                }
                rhs_data = cond.rhs_val.raw->data;
            } else {
                auto rhs_it = get_col(cols_, cond.rhs_col);
                rhs_data = get_col_data(rhs_it->offset);
            }
            
            if (!compare_values(lhs_data, rhs_data, lhs_it->type, lhs_it->len, cond.op)) {
                return false;
            }
        }
        return true;
    }

    bool compare_values(const char *lhs, const char *rhs, ColType type, int len, CompOp op) {
        int cmp = 0;
        if (type == TYPE_INT) {
            int l = *reinterpret_cast<const int*>(lhs);
            int r = *reinterpret_cast<const int*>(rhs);
            cmp = (l < r) ? -1 : (l > r) ? 1 : 0;
        } else if (type == TYPE_BIGINT) {
            int64_t l = *reinterpret_cast<const int64_t*>(lhs);
            int64_t r = *reinterpret_cast<const int64_t*>(rhs);
            cmp = (l < r) ? -1 : (l > r) ? 1 : 0;
        } else if (type == TYPE_FLOAT) {
            float l = *reinterpret_cast<const float*>(lhs);
            float r = *reinterpret_cast<const float*>(rhs);
            cmp = (l < r) ? -1 : (l > r) ? 1 : 0;
        } else if (type == TYPE_STRING || type == TYPE_DATETIME) {
            std::string l(lhs, len);
            std::string r(rhs, len);
            l.resize(strlen(l.c_str()));
            r.resize(strlen(r.c_str()));
            cmp = l.compare(r);
        }
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
        }
        return false;
    }

    // 根据全局 offset 判断该字段属于左表还是右表
    char* get_col_data(int offset) {
        if (offset < (int)left_->tupleLen()) {
            // 左表字段，从 left_rec_ 读取
            return left_rec_->data + offset;
        } else {
            // 右表字段，从 right_rec_ 读取，offset 需减去左表长度
            return right_rec_->data + (offset - (int)left_->tupleLen());
        }
    }
};