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
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    AggregateInfo agg_info_;
    std::vector<ColMeta> cols_;
    size_t len_;
    bool done_;
    std::unique_ptr<RmRecord> result_;

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, AggregateInfo agg_info) {
        prev_ = std::move(prev);
        agg_info_ = agg_info;
        len_ = (agg_info_.out_type == TYPE_INT ? sizeof(int)
                : agg_info_.out_type == TYPE_BIGINT ? sizeof(int64_t)
                : agg_info_.out_type == TYPE_FLOAT ? sizeof(float) : sizeof(int));
        ColMeta col;
        col.name = agg_info_.alias.empty() ? get_default_agg_name(agg_info_.agg_type) : agg_info_.alias;
        col.type = agg_info_.out_type;
        col.len = len_;
        col.offset = 0;
        col.tab_name = agg_info_.col.tab_name;
        cols_.push_back(col);
        done_ = false;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override {
        done_ = false;
        compute();
    }

    void nextTuple() override { done_ = true; }

    bool is_end() const override { return done_; }

    std::unique_ptr<RmRecord> Next() override {
        if (done_) return nullptr;
        return std::move(result_);
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    void compute() {
        // 找到输入列
        const std::vector<ColMeta> &prev_cols = prev_->cols();
        int col_idx = -1;
        if (agg_info_.agg_type != AGG_COUNT_STAR) {
            for (size_t i = 0; i < prev_cols.size(); ++i) {
                if (prev_cols[i].tab_name == agg_info_.col.tab_name &&
                    prev_cols[i].name == agg_info_.col.col_name) {
                    col_idx = (int)i;
                    break;
                }
            }
        }

        result_ = std::make_unique<RmRecord>(len_);
        memset(result_->data, 0, len_);

        if (agg_info_.agg_type == AGG_COUNT || agg_info_.agg_type == AGG_COUNT_STAR) {
            int count = 0;
            for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
                count++;
            }
            *(int *)result_->data = count;
            return;
        }

        bool first = true;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            char *col_data = rec->data + prev_cols[col_idx].offset;
            ColType type = prev_cols[col_idx].type;

            if (agg_info_.agg_type == AGG_SUM) {
                if (type == TYPE_INT) {
                    int val = *(int *)col_data;
                    if (first) {
                        *(int64_t *)result_->data = val;
                        first = false;
                    } else {
                        *(int64_t *)result_->data += val;
                    }
                } else if (type == TYPE_BIGINT) {
                    int64_t val = *(int64_t *)col_data;
                    if (first) {
                        *(int64_t *)result_->data = val;
                        first = false;
                    } else {
                        *(int64_t *)result_->data += val;
                    }
                } else if (type == TYPE_FLOAT) {
                    float val = *(float *)col_data;
                    if (first) {
                        *(float *)result_->data = val;
                        first = false;
                    } else {
                        *(float *)result_->data += val;
                    }
                }
            } else if (agg_info_.agg_type == AGG_MAX) {
                if (type == TYPE_INT) {
                    int val = *(int *)col_data;
                    if (first || val > *(int *)result_->data) {
                        *(int *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_BIGINT) {
                    int64_t val = *(int64_t *)col_data;
                    if (first || val > *(int64_t *)result_->data) {
                        *(int64_t *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_FLOAT) {
                    float val = *(float *)col_data;
                    if (first || val > *(float *)result_->data) {
                        *(float *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_STRING || type == TYPE_DATETIME) {
                    std::string val(col_data, prev_cols[col_idx].len);
                    std::string cur(result_->data, prev_cols[col_idx].len);
                    if (first || val > cur) {
                        memcpy(result_->data, col_data, prev_cols[col_idx].len);
                        first = false;
                    }
                }
            } else if (agg_info_.agg_type == AGG_MIN) {
                if (type == TYPE_INT) {
                    int val = *(int *)col_data;
                    if (first || val < *(int *)result_->data) {
                        *(int *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_BIGINT) {
                    int64_t val = *(int64_t *)col_data;
                    if (first || val < *(int64_t *)result_->data) {
                        *(int64_t *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_FLOAT) {
                    float val = *(float *)col_data;
                    if (first || val < *(float *)result_->data) {
                        *(float *)result_->data = val;
                        first = false;
                    }
                } else if (type == TYPE_STRING || type == TYPE_DATETIME) {
                    std::string val(col_data, prev_cols[col_idx].len);
                    std::string cur(result_->data, prev_cols[col_idx].len);
                    if (first || val < cur) {
                        memcpy(result_->data, col_data, prev_cols[col_idx].len);
                        first = false;
                    }
                }
            }
        }
    }
};
