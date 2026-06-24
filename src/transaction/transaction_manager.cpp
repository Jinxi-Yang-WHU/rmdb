/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

static void flush_all_dirty_pages(SmManager *sm_manager) {
    auto bpm = sm_manager->get_bpm();
    for (auto &entry : sm_manager->fhs_) {
        bpm->flush_all_pages(entry.second->get_fd());
        entry.second->flush_file_hdr();
    }
    for (auto &entry : sm_manager->ihs_) {
        bpm->flush_all_pages(entry.second->get_fd());
        entry.second->flush_file_hdr();
    }
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // 1. 如果传入空指针，创建新事务
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_++;
        timestamp_t start_ts = next_timestamp_++;
        txn = new Transaction(txn_id);
        txn->set_start_ts(start_ts);
    }
    
    // 2. 设置事务状态为 GROWING（两阶段锁的增长阶段）
    txn->set_state(TransactionState::GROWING);
    
    // 3. 加入全局事务表
    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    lock.unlock();

    // 4. 写入 begin 日志
    if (log_manager != nullptr) {
        BeginLogRecord begin_rec(txn->get_transaction_id());
        auto lsn = log_manager->add_log_to_buffer(&begin_rec);
        txn->set_prev_lsn(lsn);
    }
    
    // 5. 返回事务指针
    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // 1. 写入 commit 日志并刷盘，保证持久性
    if (log_manager != nullptr) {
        CommitLogRecord commit_rec(txn->get_transaction_id());
        auto lsn = log_manager->add_log_to_buffer(&commit_rec);
        txn->set_prev_lsn(lsn);
        log_manager->flush_log_to_disk();
    }

    // 2. 把脏页刷盘（简单 FORCE 策略，确保提交后的数据不会丢失）
    flush_all_dirty_pages(sm_manager_);

    // 3. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
    
    // 4. 清空写操作集（释放内存）
    auto write_set = txn->get_write_set();
    for (auto &write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    
    // 5. 清空事务相关资源
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();
    
    // 6. 更新事务状态
    txn->set_state(TransactionState::COMMITTED);
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // 1. 写入 abort 日志
    if (log_manager != nullptr) {
        AbortLogRecord abort_rec(txn->get_transaction_id());
        auto lsn = log_manager->add_log_to_buffer(&abort_rec);
        txn->set_prev_lsn(lsn);
    }

    // 2. 回滚所有写操作（逆序遍历，先回滚后执行的操作）
    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        auto &write_record = *it;
        auto wtype = write_record->GetWriteType();
        auto &tab_name = write_record->GetTableName();
        auto &rid = write_record->GetRid();
        
        if (wtype == WType::INSERT_TUPLE) {
            // 回滚插入：删除已插入的记录
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            fh->delete_record(rid, nullptr);
        } else if (wtype == WType::DELETE_TUPLE || wtype == WType::UPDATE_TUPLE) {
            // 回滚删除/更新：恢复旧记录
            auto fh = sm_manager_->fhs_.at(tab_name).get();
            auto &record = write_record->GetRecord();
            fh->update_record(rid, record.data, nullptr);
        }
    }
    
    // 3. 把回滚后的脏页刷盘
    flush_all_dirty_pages(sm_manager_);

    // 4. 把 abort 日志刷盘
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }

    // 5. 释放所有锁
    auto lock_set = txn->get_lock_set();
    for (auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();
    
    // 6. 清空事务相关资源
    for (auto &write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    
    txn->get_index_latch_page_set()->clear();
    txn->get_index_deleted_page_set()->clear();
    
    // 7. 更新事务状态
    txn->set_state(TransactionState::ABORTED);
}
