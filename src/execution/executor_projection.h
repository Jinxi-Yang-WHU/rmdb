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

// 投影算子 对于 SELECT col1, col2, ... FROM table_name 的查询，返回指定字段的记录
// 火山模型，每一个算子提供统一的接口。
class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 投影后一条记录的总字节长度
    std::vector<size_t> sel_idxs_;                  // 需要投影的字段在儿子节点的字段列表中的索引

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();   // 获取上游算子的字段列表
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());  // 记录该列在上游中的下标（0, 1, 2...）

            // 复制该列的元数据，但重新计算 offset（因为输出记录是紧凑排列的）
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset; // 所有选中列的长度之和
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    void beginTuple() override { prev_->beginTuple(); }

    void nextTuple() override { prev_->nextTuple(); }

    bool is_end() const override { return prev_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        // 1. 如果上游已经遍历结束，返回空
        if (prev_->is_end()) return nullptr;
        
        // 2. 从上游取完整记录（包含所有字段）
        auto prev_rec = prev_->Next();
        
        // 3. 创建一个新的定长记录，长度是投影后所有选中列之和
        auto rec = std::make_unique<RmRecord>(len_);
        
        // 4. 逐列拷贝：从上游记录的对应位置，拷贝到新记录的紧凑位置
        size_t offset = 0;
        for (size_t idx : sel_idxs_) {
            const auto &col = prev_->cols()[idx];        // 获取上游该列的元数据
            memcpy(rec->data + offset,                    // 新记录的写入位置
                   prev_rec->data + col.offset,           // 上游记录的读取位置
                   col.len);                              // 拷贝字节数
            offset += col.len;                            // 新位置向后移动
        }
        
        return rec;
    }

    Rid &rid() override { return _abstract_rid; }
};