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

// 全表顺序扫描算子 对于 SELECT * FROM table_name WHERE condition 的查询，直接扫描表中所有记录，返回满足条件的记录
/*
功能：
打开表文件，逐条遍历所有记录；
对每条记录求值 WHERE 条件（conds_）；
只把满足条件的记录暴露给上层算子。
实现：
采用火山模型，每一个算子提供统一的接口。
*/
class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄，用于读写记录文件
    std::vector<ColMeta> cols_;         // 表的字段元数据
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;                           // 当前记录的物理位置（page_no, slot_no）
    std::unique_ptr<RecScan> scan_;     // table_iterator，底层通过 Bitmap 跳过空

    SmManager *sm_manager_;             // 系统管理器，用于获取表元数据和文件句柄
    bool is_end_;

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        // 计算记录长度：最后一个字段的 offset + len
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        // 1. 创建 RmScan 遍历器，它会自动定位到第一条有效记录（Bitmap=1 的 slot）
        scan_ = std::make_unique<RmScan>(fh_);
        
        // 2. 检查是否已经到达文件末尾
        is_end_ = scan_->is_end();
        
        // 3. 如果文件非空，检查第一条记录是否满足 WHERE 条件
        if (!is_end_) {
            rid_ = scan_->rid();                    // 获取当前记录的物理位置
            auto rec = fh_->get_record(rid_, context_);  // 从 BufferPool 读取记录
            if (!eval_conds(rec.get())) {          // 如果不满足条件，递归继续找
                nextTuple();
            }
        }        
    }

    void nextTuple() override {
        // 1. 先让 RmScan 向后移动一步（跳到下一个 Bitmap=1 的 slot）
        scan_->next();
        
        // 2. 循环：只要文件没结束，就检查当前记录是否满足条件
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            
            // 找到满足条件的记录，停止循环，is_end_ 设为 false
            if (eval_conds(rec.get())) {
                is_end_ = false;
                return;
            }
            
            // 不满足，继续往后走
            scan_->next();
        }
        
        // 3. 走到这里说明文件遍历完了，也没找到满足条件的记录
        is_end_ = true;        
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) return nullptr;
        // 通过文件句柄重新读取当前 rid 的记录并返回
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return is_end_; }

    Rid &rid() override { return rid_; }

   private:
    // 对单条记录求值所有 WHERE 条件，全部满足才返回 true
    bool eval_conds(const RmRecord *rec) {
        for (auto &cond : fed_conds_) {
            // 1. 在 cols_ 中找到条件左操作数对应的字段
            auto lhs_it = get_col(cols_, cond.lhs_col);
            char *lhs_data = rec->data + lhs_it->offset;  // 计算该字段在记录中的地址
            
            char *rhs_data = nullptr;
            Value rhs_val;
            
            // 2. 右操作数可能是常量（如 90），也可能是另一列（如 t2.id）
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    Value rhs_val = cond.rhs_val;
                    rhs_val.init_raw(lhs_it->len);
                    rhs_data = rhs_val.raw->data;
                } else {
                    rhs_data = cond.rhs_val.raw->data;
                }
            } else {
                auto rhs_it = get_col(cols_, cond.rhs_col);
                rhs_data = rec->data + rhs_it->offset;
            }
            
            // 3. 比较左右操作数
            if (!compare_values(lhs_data, rhs_data, lhs_it->type, cond.op)) {
                return false;  // 有一个条件不满足，整体为 false
            }
        }
        return true;  // 所有条件都满足
    }
    
    bool compare_values(const char *lhs, const char *rhs, ColType type, CompOp op) {
        int cmp = 0;  // -1: 小于, 0: 等于, 1: 大于
        
        if (type == TYPE_INT) {
            int l = *reinterpret_cast<const int*>(lhs);
            int r = *reinterpret_cast<const int*>(rhs);
            cmp = (l < r) ? -1 : (l > r) ? 1 : 0;
        } else if (type == TYPE_FLOAT) {
            float l = *reinterpret_cast<const float*>(lhs);
            float r = *reinterpret_cast<const float*>(rhs);
            cmp = (l < r) ? -1 : (l > r) ? 1 : 0;
        } else if (type == TYPE_STRING) {
            // 定长字符串，由于 init_raw 用 \0 填充了多余字节，
            // 所以直接用 strcmp 即可，遇到 \0 就会停止
            cmp = strcmp(lhs, rhs);
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
};