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
DeleteExecutor 不遍历、不过滤、不返回记录。它只接收一个已经收集好的 rids_ 列表，在 Next() 中一次性完成全部删除工作。
*/
class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;                   // 表的元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::vector<Rid> rids_;         // 需要删除的记录的位置
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name); // 从元数据获取表结构
        fh_ = sm_manager_->fhs_.at(tab_name).get(); // 获取已打开的数据文件句柄
        conds_ = conds;
        rids_ = rids; // 上层已收集好的目标记录位置
        context_ = context;
    }

    std::unique_ptr<RmRecord> Next() override {
        // 遍历所有待删除的记录位置
        for (auto &rid : rids_) {
            
            // 1. 如果表上有索引，必须先删索引项
            if (!tab_.indexes.empty()) {
                // 读取旧记录，用于构造索引 key
                auto rec = fh_->get_record(rid, context_);
                
                // 遍历该表上的所有索引
                for (auto &index : tab_.indexes) {
                    // 获取索引文件名（如 "grade_id"）
                    auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
                    
                    // 如果该索引文件已打开（在 ihs_ 中）
                    if (sm_manager_->ihs_.find(index_name) != sm_manager_->ihs_.end()) {
                        auto ih = sm_manager_->ihs_.at(index_name).get();
                        
                        // 构造索引 key：把索引包含的列按顺序拼接到 key 缓冲区
                        char* key = new char[index.col_tot_len];
                        int offset = 0;
                        for (int i = 0; i < index.col_num; ++i) {
                            memcpy(key + offset, 
                                   rec->data + index.cols[i].offset, 
                                   index.cols[i].len);
                            offset += index.cols[i].len;
                        }
                        
                        // 从 B+ 树中删除该 key 对应的 rid
                        ih->delete_entry(key, context_->txn_);
                        delete[] key;
                    }
                }
            }
            
            // 2. 从数据文件中删除记录（Bitmap 对应 bit 置 0）
            fh_->delete_record(rid, context_);
        }
        
        // DML 算子不返回记录，返回 nullptr
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};