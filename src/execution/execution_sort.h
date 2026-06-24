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
#include <algorithm>
#include <cstring>

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    // 排序键：每一键对应的列元数据以及是否降序
    std::vector<std::pair<ColMeta, bool>> sort_keys_;
    int limit_ = -1;
    std::vector<RmRecord> tuples_;
    size_t idx_ = 0;

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev,
                 const std::vector<std::pair<TabCol, bool>> &sort_keys,
                 int limit = -1) {
        prev_ = std::move(prev);
        limit_ = limit;
        for (const auto &sk : sort_keys) {
            auto pos = get_col(prev_->cols(), sk.first);
            sort_keys_.emplace_back(*pos, sk.second);
        }
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    void beginTuple() override {
        tuples_.clear();
        idx_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            tuples_.emplace_back(*rec);
        }

        std::sort(tuples_.begin(), tuples_.end(),
                  [&](const RmRecord &a, const RmRecord &b) {
                      for (const auto &sk : sort_keys_) {
                          const ColMeta &col = sk.first;
                          bool desc = sk.second;
                          char *da = a.data + col.offset;
                          char *db = b.data + col.offset;
                          int cmp = 0;
                          if (col.type == TYPE_INT) {
                              int va = *(int *)da;
                              int vb = *(int *)db;
                              cmp = (va < vb) ? -1 : (va > vb);
                          } else if (col.type == TYPE_BIGINT) {
                              int64_t va = *(int64_t *)da;
                              int64_t vb = *(int64_t *)db;
                              cmp = (va < vb) ? -1 : (va > vb);
                          } else if (col.type == TYPE_FLOAT) {
                              float va = *(float *)da;
                              float vb = *(float *)db;
                              cmp = (va < vb) ? -1 : (va > vb);
                          } else if (col.type == TYPE_STRING || col.type == TYPE_DATETIME) {
                              std::string sa(da, col.len);
                              sa.resize(strlen(sa.c_str()));
                              std::string sb(db, col.len);
                              sb.resize(strlen(sb.c_str()));
                              cmp = sa.compare(sb);
                          }
                          if (cmp != 0) {
                              return desc ? (cmp > 0) : (cmp < 0);
                          }
                      }
                      return false;
                  });

        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(limit_);
        }
    }

    void nextTuple() override { ++idx_; }

    bool is_end() const override { return idx_ >= tuples_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end()) return nullptr;
        return std::make_unique<RmRecord>(tuples_[idx_]);
    }

    Rid &rid() override { return _abstract_rid; }
};
