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

//只接收 rids_ 列表和 set_clauses_，在 Next() 中一次性完成全部更新。
class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                    // 表元数据
    std::vector<Condition> conds_;   // WHERE 条件（本算子内不直接使用）
    RmFileHandle *fh_;               // 表数据文件句柄
    std::vector<Rid> rids_;          // 待更新记录的物理位置（上层扫描收集）
    std::string tab_name_;           // 表名称
    std::vector<SetClause> set_clauses_;  // SET 子句列表，如 [score=score+5, name='Error']
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
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);
            char *data = rec->data;

            for (auto &set_clause : set_clauses_) {
                auto lhs_col = tab_.get_col(set_clause.lhs.col_name);
                char *lhs_ptr = data + lhs_col->offset;

                if (set_clause.is_rhs_expr) {
                    auto rhs_col = tab_.get_col(set_clause.rhs_col.col_name);
                    char *rhs_ptr = data + rhs_col->offset;

                    if (lhs_col->type == TYPE_INT) {
                        int rhs_val = *(int *)rhs_ptr;
                        int const_val = set_clause.rhs_expr_val.int_val;
                        int result = (set_clause.arith_op == ARITH_ADD) ? (rhs_val + const_val)
                                                                      : (rhs_val - const_val);
                        *(int *)lhs_ptr = result;
                    } else if (lhs_col->type == TYPE_BIGINT) {
                        int64_t rhs_val = *(int64_t *)rhs_ptr;
                        int64_t const_val = set_clause.rhs_expr_val.bigint_val;
                        int64_t result = (set_clause.arith_op == ARITH_ADD) ? (rhs_val + const_val)
                                                                        : (rhs_val - const_val);
                        *(int64_t *)lhs_ptr = result;
                    } else if (lhs_col->type == TYPE_FLOAT) {
                        float rhs_val = *(float *)rhs_ptr;
                        float const_val = set_clause.rhs_expr_val.float_val;
                        float result = (set_clause.arith_op == ARITH_ADD) ? (rhs_val + const_val)
                                                                          : (rhs_val - const_val);
                        *(float *)lhs_ptr = result;
                    } else {
                        throw InternalError("String type does not support arithmetic operations in SET clause");
                    }
                } else {
                    if (set_clause.rhs.raw == nullptr) {
                        set_clause.rhs.init_raw(lhs_col->len);
                    }
                    memcpy(lhs_ptr, set_clause.rhs.raw->data, lhs_col->len);
                }
            }

            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();

                std::vector<char> old_key(index.col_tot_len);
                int offset = 0;
                for (size_t i = 0; i < (size_t)index.col_num; ++i) {
                    memcpy(old_key.data() + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->delete_entry(old_key.data(), context_->txn_);

                std::vector<char> new_key(index.col_tot_len);
                offset = 0;
                for (size_t i = 0; i < (size_t)index.col_num; ++i) {
                    memcpy(new_key.data() + offset, data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->insert_entry(new_key.data(), rid, context_->txn_);
            }

            fh_->update_record(rid, data, context_);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};