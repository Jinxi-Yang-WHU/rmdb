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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<IxScan> scan_;
    bool is_end_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
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
        is_end_ = true;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        
        // 构造索引搜索key：只使用索引列的等值条件
        std::vector<char> key(index_meta_.col_tot_len);
        int offset = 0;
        for (size_t i = 0; i < index_col_names_.size(); ++i) {
            auto &col_name = index_col_names_[i];
            auto &index_col = index_meta_.cols[i];
            bool found = false;
            for (auto &cond : fed_conds_) {
                if (cond.lhs_col.col_name == col_name && cond.is_rhs_val && cond.op == OP_EQ) {
                    if (cond.rhs_val.raw == nullptr) {
                        cond.rhs_val.init_raw(index_col.len);
                    }
                    memcpy(key.data() + offset, cond.rhs_val.raw->data, index_col.len);
                    found = true;
                    break;
                }
            }
            if (!found) {
                is_end_ = true;
                return;
            }
            offset += index_col.len;
        }

        Iid lower = ih->lower_bound(key.data());
        Iid upper = ih->upper_bound(key.data());
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        
        is_end_ = scan_->is_end();
        if (!is_end_) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (!eval_conds(rec.get())) {
                nextTuple();
            }
        }
    }

    void nextTuple() override {
        while (!scan_->is_end()) {
            scan_->next();
            if (scan_->is_end()) {
                is_end_ = true;
                return;
            }
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(rec.get())) {
                is_end_ = false;
                return;
            }
        }
        is_end_ = true;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) return nullptr;
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return is_end_; }

    Rid &rid() override { return rid_; }

   private:
    bool eval_conds(const RmRecord *rec) {
        for (auto &cond : fed_conds_) {
            auto lhs_it = get_col(cols_, cond.lhs_col);
            char *lhs_data = rec->data + lhs_it->offset;
            char *rhs_data = nullptr;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    cond.rhs_val.init_raw(lhs_it->len);
                }
                rhs_data = cond.rhs_val.raw->data;
            } else {
                auto rhs_it = get_col(cols_, cond.rhs_col);
                rhs_data = rec->data + rhs_it->offset;
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
};
