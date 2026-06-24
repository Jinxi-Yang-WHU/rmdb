/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include "ix_scan.h"

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 */
int IxNodeHandle::lower_bound(const char *target) const {
    int left = 0, right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp >= 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 */
int IxNodeHandle::upper_bound(const char *target) const {
    int left = 0, right = page_hdr->num_key;
    while (left < right) {
        int mid = (left + right) / 2;
        int cmp = ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_);
        if (cmp > 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    int pos = upper_bound(key);
    return value_at(pos);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= page_hdr->num_key);
    assert(page_hdr->num_key + n <= get_max_size());
    int num = page_hdr->num_key;
    int col_len = file_hdr->col_tot_len_;
    // 移动key
    if (num - pos > 0) {
        memmove(keys + (pos + n) * col_len, keys + pos * col_len, (num - pos) * col_len);
    }
    // 移动rid
    if (num - pos > 0) {
        memmove(rids + pos + n, rids + pos, (num - pos) * sizeof(Rid));
    }
    // 插入新key
    for (int i = 0; i < n; ++i) {
        memcpy(keys + (pos + i) * col_len, key + i * col_len, col_len);
    }
    // 插入新rid
    for (int i = 0; i < n; ++i) {
        rids[pos + i] = rid[i];
    }
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        // key重复，不插入
        return page_hdr->num_key;
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 */
void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < page_hdr->num_key);
    int num = page_hdr->num_key;
    int col_len = file_hdr->col_tot_len_;
    if (num - pos - 1 > 0) {
        memmove(keys + pos * col_len, keys + (pos + 1) * col_len, (num - pos - 1) * col_len);
        memmove(rids + pos, rids + pos + 1, (num - pos - 1) * sizeof(Rid));
    }
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key && ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // init file_hdr_
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;
    
    // disk_manager管理的fd对应的文件中，设置从file_hdr_->num_pages开始分配page_no
    int now_page_no = disk_manager_->get_fd2pageno(fd);
    disk_manager_->set_fd2pageno(fd, now_page_no + 1);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) {
    IxNodeHandle *node = fetch_node(file_hdr_->root_page_);
    while (!node->is_leaf_page()) {
        page_id_t next_page_no = node->internal_lookup(key);
        IxNodeHandle *next_node = fetch_node(next_page_no);
        buffer_pool_manager_->unpin_page(node->get_page_id(), false);
        node = next_node;
    }
    return std::make_pair(node, false);
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 */
bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction) {
    IxNodeHandle *leaf = find_leaf_page(key, Operation::FIND, transaction).first;
    Rid *rid = nullptr;
    bool found = leaf->leaf_lookup(key, &rid);
    if (found) {
        result->push_back(*rid);
    }
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node) {
    IxNodeHandle *new_node = create_node();
    int size = node->get_size();
    int split_pos = size / 2;
    int col_len = file_hdr_->col_tot_len_;

    if (node->is_leaf_page()) {
        // 叶子节点：右半部分移动到new_node
        int new_size = size - split_pos;
        new_node->insert_pairs(0, node->get_key(split_pos), node->get_rid(split_pos), new_size);
        node->set_size(split_pos);

        // 更新leaf链表
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());
        node->set_next_leaf(new_node->get_page_no());
        if (new_node->get_next_leaf() != IX_NO_PAGE) {
            IxNodeHandle *next = fetch_node(new_node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // 内部节点：右半部分key/rid移动到new_node
        int new_key_size = size - split_pos - 1;
        // 移动key[split_pos+1 .. size-1] 和 rid[split_pos+1 .. size-1]
        new_node->insert_pairs(0, node->get_key(split_pos + 1), node->get_rid(split_pos + 1), new_key_size);
        // 移动最后一个rid
        new_node->set_rid(new_key_size, *node->get_rid(size));
        new_node->set_size(new_key_size);
        node->set_size(split_pos);

        // 更新new_node中所有孩子的父节点
        for (int i = 0; i <= new_key_size; ++i) {
            maintain_child(new_node, i);
        }
    }

    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                     Transaction *transaction) {
    if (old_node->is_root_page()) {
        // 创建新根
        IxNodeHandle *new_root = create_node();
        new_root->set_parent_page_no(IX_NO_PAGE);
        new_root->page_hdr->is_leaf = false;
        // 新根：rid[0]=old_node, key[0]=key, rid[1]=new_node
        new_root->set_size(0);
        new_root->set_rid(0, Rid{old_node->get_page_no(), -1});
        new_root->set_rid(1, Rid{new_node->get_page_no(), -1});
        memcpy(new_root->get_key(0), key, file_hdr_->col_tot_len_);
        new_root->set_size(1);

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());

        update_root_page_no(new_root->get_page_no());

        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return;
    }

    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int pos = parent->find_child(old_node);
    // 在parent的pos之后插入(key, new_node.page_no)
    parent->insert_pair(pos + 1, key, Rid{new_node->get_page_no(), -1});
    new_node->set_parent_page_no(parent->get_page_no());

    if (parent->get_size() == parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent);
        // new_parent的第一个key提升到parent的parent
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
        buffer_pool_manager_->unpin_page(new_parent->get_page_id(), true);
    }
    buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
}

/**
 * @brief 将指定键值对插入到B+树中
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction) {
    if (is_empty()) {
        return -1;
    }

    IxNodeHandle *leaf = find_leaf_page(key, Operation::INSERT, transaction).first;
    int old_size = leaf->get_size();
    int new_size = leaf->insert(key, value);
    if (new_size == old_size) {
        // 重复key，未插入
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return -1;
    }

    if (leaf->get_size() == leaf->get_max_size()) {
        IxNodeHandle *new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
        buffer_pool_manager_->unpin_page(new_leaf->get_page_id(), true);
    }

    page_id_t leaf_page_no = leaf->get_page_no();
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    return leaf_page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction) {
    IxNodeHandle *leaf = find_leaf_page(key, Operation::DELETE, transaction).first;
    int old_size = leaf->get_size();
    int new_size = leaf->remove(key);
    if (new_size == old_size) {
        // 未找到key
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
        return false;
    }

    bool root_is_latched = false;
    bool need_delete = coalesce_or_redistribute(leaf, transaction, &root_is_latched);
    if (!need_delete) {
        buffer_pool_manager_->unpin_page(leaf->get_page_id(), true);
    }
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched) {
    if (node->is_root_page()) {
        return adjust_root(node);
    }

    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);
    IxNodeHandle *neighbor_node = nullptr;
    int neighbor_index = -1;

    if (index > 0) {
        // 优先前驱兄弟
        neighbor_index = index - 1;
        neighbor_node = fetch_node(parent->value_at(neighbor_index));
    } else {
        // 没有前驱，取后继
        neighbor_index = index + 1;
        neighbor_node = fetch_node(parent->value_at(neighbor_index));
    }

    bool parent_should_delete = false;
    if (node->get_size() + neighbor_node->get_size() >= node->get_min_size() * 2) {
        redistribute(neighbor_node, node, parent, index);
        buffer_pool_manager_->unpin_page(neighbor_node->get_page_id(), true);
        buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
        return false;
    } else {
        parent_should_delete = coalesce(&neighbor_node, &node, &parent, index, transaction, root_is_latched);
        buffer_pool_manager_->unpin_page(neighbor_node->get_page_id(), true);
        if (parent_should_delete) {
            // parent需要被删除或调整，递归处理
            bool grandparent_should_delete = coalesce_or_redistribute(parent, transaction, root_is_latched);
            if (!grandparent_should_delete) {
                buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            }
            return true;
        } else {
            buffer_pool_manager_->unpin_page(parent->get_page_id(), true);
            return true; // node被删除了
        }
    }
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node) {
    if (old_root_node->is_leaf_page()) {
        // 根是叶子，即使大小为0也不需要删除根
        return false;
    }

    if (old_root_node->get_size() == 1) {
        // 内部根节点只有一个key，把唯一孩子提升为新根
        page_id_t new_root_page_no = old_root_node->value_at(0);
        IxNodeHandle *new_root = fetch_node(new_root_page_no);
        new_root->set_parent_page_no(IX_NO_PAGE);
        update_root_page_no(new_root_page_no);
        if (new_root->is_leaf_page()) {
            file_hdr_->first_leaf_ = new_root_page_no;
            file_hdr_->last_leaf_ = new_root_page_no;
        }
        buffer_pool_manager_->unpin_page(new_root->get_page_id(), true);
        return true;
    }
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index) {
    int col_len = file_hdr_->col_tot_len_;
    if (index == 0) {
        // neighbor是后继（右兄弟），把neighbor的第一个键值对移到node末尾
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);

        // 更新parent中node的key（node在parent的index=0处，其key在parent的key[0]）
        memcpy(parent->get_key(0), neighbor_node->get_key(0), col_len);
    } else {
        // neighbor是前驱（左兄弟），把neighbor的最后一个键值对移到node开头
        node->insert_pair(0, neighbor_node->get_key(neighbor_node->get_size() - 1),
                          *neighbor_node->get_rid(neighbor_node->get_size() - 1));
        neighbor_node->erase_pair(neighbor_node->get_size() - 1);

        // 更新parent中node的key（node在parent的index处，其key在parent的key[index-1]）
        memcpy(parent->get_key(index - 1), node->get_key(0), col_len);
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并
 */
bool IxIndexHandle::coalesce(IxNodeHandle **neighbor_node, IxNodeHandle **node, IxNodeHandle **parent, int index,
                             Transaction *transaction, bool *root_is_latched) {
    // 确保neighbor_node是左兄弟，node是右兄弟
    if (index == 0) {
        std::swap(*neighbor_node, *node);
        index = 1;
    }

    IxNodeHandle *left = *neighbor_node;
    IxNodeHandle *right = *node;
    int left_size = left->get_size();
    int right_size = right->get_size();

    if (right->is_leaf_page()) {
        // 叶子节点：把right的键值对全部移到left
        left->insert_pairs(left_size, right->get_key(0), right->get_rid(0), right_size);
        left->set_next_leaf(right->get_next_leaf());
        if (right->get_next_leaf() != IX_NO_PAGE) {
            IxNodeHandle *next = fetch_node(right->get_next_leaf());
            next->set_prev_leaf(left->get_page_no());
            buffer_pool_manager_->unpin_page(next->get_page_id(), true);
        }
        if (file_hdr_->last_leaf_ == right->get_page_no()) {
            file_hdr_->last_leaf_ = left->get_page_no();
        }
    } else {
        // 内部节点：需要把parent中分隔left和right的key拿下来
        char *split_key = (*parent)->get_key(index - 1);
        left->insert_pair(left_size, split_key, *right->get_rid(0));
        // 再把right剩余的key/rid移过去
        if (right_size > 0) {
            left->insert_pairs(left_size + 1, right->get_key(0), right->get_rid(1), right_size);
        }
        // 更新孩子的父节点
        for (int i = left_size + 1; i <= left->get_size(); ++i) {
            maintain_child(left, i);
        }
    }

    // 删除right节点
    release_node_handle(*right);
    buffer_pool_manager_->unpin_page(right->get_page_id(), true);
    buffer_pool_manager_->delete_page(right->get_page_id());

    // 从parent中删除right对应的entry
    (*parent)->erase_pair(index);

    // 如果parent是根且size为0，需要adjust_root；否则如果parent size < min_size，需要继续合并
    if ((*parent)->is_root_page()) {
        return (*parent)->get_size() == 0;
    }
    return (*parent)->get_size() < (*parent)->get_min_size();
}

/**
 * @brief 这里把iid转换成了rid
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    IxNodeHandle *node = fetch_node(iid.page_no);
    if (iid.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return *node->get_rid(iid.slot_no);
}

/**
 * @brief FindLeafPage + lower_bound
 */
Iid IxIndexHandle::lower_bound(const char *key) {
    IxNodeHandle *leaf = find_leaf_page(key, Operation::FIND, nullptr).first;
    int pos = leaf->lower_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

/**
 * @brief FindLeafPage + upper_bound
 */
Iid IxIndexHandle::upper_bound(const char *key) {
    IxNodeHandle *leaf = find_leaf_page(key, Operation::FIND, nullptr).first;
    int pos = leaf->upper_bound(key);
    Iid iid = {.page_no = leaf->get_page_no(), .slot_no = pos};
    buffer_pool_manager_->unpin_page(leaf->get_page_id(), false);
    return iid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 */
Iid IxIndexHandle::leaf_end() const {
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    buffer_pool_manager_->unpin_page(node->get_page_id(), false);  // unpin it!
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 */
Iid IxIndexHandle::leaf_begin() const {
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    
    return node;
}

/**
 * @brief 创建一个新结点
 */
IxNodeHandle *IxIndexHandle::create_node() {
    IxNodeHandle *node;
    file_hdr_->num_pages_++;

    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    // 从3开始分配page_no，第一次分配之后，new_page_id.page_no=3，file_hdr_.num_pages=4
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    node = new IxNodeHandle(file_hdr_, page);
    return node;
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node) {
    IxNodeHandle *curr = node;
    while (curr->get_parent_page_no() != IX_NO_PAGE) {
        // Load its parent
        IxNodeHandle *parent = fetch_node(curr->get_parent_page_no());
        int rank = parent->find_child(curr);
        char *parent_key = parent->get_key(rank);
        char *child_first_key = curr->get_key(0);
        if (memcmp(parent_key, child_first_key, file_hdr_->col_tot_len_) == 0) {
            assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
            break;
        }
        memcpy(parent_key, child_first_key, file_hdr_->col_tot_len_);  // 修改了parent node
        curr = parent;

        assert(buffer_pool_manager_->unpin_page(parent->get_page_id(), true));
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    buffer_pool_manager_->unpin_page(prev->get_page_id(), true);

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    buffer_pool_manager_->unpin_page(next->get_page_id(), true);
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    file_hdr_->num_pages_--;
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        buffer_pool_manager_->unpin_page(child->get_page_id(), true);
    }
}
