#include "table.h"
#include "bpt.h"

#include <algorithm>

using namespace JiDB;

namespace JiDB {

    /******************************************/
    /************ HELPER FUNCTIONS ************/
    /******************************************/

    // id를 가지고 노드를 읽어 온다! 
    // 리턴값은 Leaf 또는 Internal 객체다. is_leaf를 확인하여 적절히 캐스팅하여 사용해야 한다.
    BPT::Node * BPT::get_node(pageid_t id) const {
        // Read page from disk manager.
        page_t page;
        disk_mgr->read(id, page);
        RAW_BPT_Page & header = *reinterpret_cast<RAW_BPT_Page *>(&page);
        Node * node = header.is_leaf ? 
                      (static_cast<Node *>(new Leaf(*disk_mgr, page, id))) :
                      (static_cast<Node *>(new Internal(*disk_mgr, page, id)));
        return node;
    }

    // Node를 RAW_BPT_Page 형태로 표현한 객체를 생성한다.
    BPT::RAW_BPT_Page::RAW_BPT_Page(const DiskMgr & disk_mgr, const Node & node) {
        parent      = (uint64_t)disk_mgr.get_offset(node.parent);
        is_leaf     = node.is_leaf;
        num_of_keys = node.num_of_keys;
    }

    BPT::RAW_Leaf_Page::RAW_Leaf_Page(const DiskMgr & disk_mgr, const Leaf & leaf) :
            RAW_BPT_Page(disk_mgr, *static_cast<const Node *>(&leaf)) {
        right_sibling = (uint64_t)disk_mgr.get_offset(leaf.right_sibling);
        memcpy(records, leaf.records, sizeof(records));
    }

    BPT::RAW_Internal_Page::RAW_Internal_Page(const DiskMgr & disk_mgr, const Internal & internal) :
            RAW_BPT_Page(disk_mgr, *static_cast<const Node*>(&internal)) {
        leftmost_page = (off_t)disk_mgr.get_offset(internal.leftmost_page);
        for (int i = 0; i < 248; i++) {
            key_off_pairs[i].key      = internal.key_ptr_pairs[i].key;
            key_off_pairs[i].nxt_page = disk_mgr.get_offset(internal.key_ptr_pairs[i].nxt_page);
        }
    }

    // 이미 있는 page를 Node 형태로 표현한 객체를 생성한다.
    BPT::Node::Node(const DiskMgr & disk_mgr, const page_t & page, pageid_t id) : id(id) {
        const RAW_BPT_Page & raw_page = *reinterpret_cast<const RAW_BPT_Page *>(&page);
        parent      = disk_mgr.get_pageid((off_t)raw_page.parent);
        is_leaf     = raw_page.is_leaf;
        num_of_keys = raw_page.num_of_keys;
    }

    BPT::Leaf::Leaf(const DiskMgr & disk_mgr, const page_t & page, pageid_t id) : Node(disk_mgr, page, id) {
        const RAW_Leaf_Page & raw_leaf = *reinterpret_cast<const RAW_Leaf_Page *>(&page);
        right_sibling = disk_mgr.get_pageid((off_t)raw_leaf.right_sibling);
        memcpy(records, raw_leaf.records, sizeof(records));
    }

    BPT::Internal::Internal(const DiskMgr & disk_mgr, const page_t & page, pageid_t id) : Node(disk_mgr, page, id) {
        const RAW_Internal_Page & raw_internal = *reinterpret_cast<const RAW_Internal_Page *>(&page);
        leftmost_page = disk_mgr.get_pageid((off_t)raw_internal.leftmost_page);
        for (int i = 0; i < 248; i++) {
            key_ptr_pairs[i].key      = raw_internal.key_off_pairs[i].key;
            key_ptr_pairs[i].nxt_page = disk_mgr.get_pageid(raw_internal.key_off_pairs[i].nxt_page);
        }
    }

    // Write this page to disk.
    void BPT::Leaf::write(const DiskMgr & disk_mgr) {
        RAW_Leaf_Page raw_leaf(disk_mgr, *this);
        disk_mgr.write(*reinterpret_cast<page_t *>(&raw_leaf));
    }

    void BPT::Internal::write(const DiskMgr & disk_mgr) {
        RAW_Internal_Page raw_internal(disk_mgr, *this);
        disk_mgr.write(*reinterpret_cast<page_t *>(&raw_internal));
    }



    /******************************************/
    /************ B+ TREE FUNCTIONS ***********/
    /******************************************/
    
    // Find value by key.
    value_t * BPT::_find(const key_t & key) {
        if (!root) return nullptr;
        Node * ptr_node = root;

        // Find leaf node.
        while (!ptr_node->is_leaf) {
            Internal & internal = *static_cast<Internal *>(ptr_node);
            pageid_t nxt_page = find_child(internal, key);
            if (ptr_node != root) delete ptr_node;
            ptr_node = get_node(nxt_page);
        }

        Leaf & leaf = *static_cast<Leaf *>(ptr_node);
        int idx = find_lower_bound_in_leaf(leaf, key);
        value_t * ret = nullptr;
        if (idx != leaf.num_of_keys && leaf.records[idx].key == key) {
            ret = new value_t;
            memcpy(ret, &leaf.records[idx].value, sizeof(value_t));
        }
        delete ptr_node;

        return ret;
    }

    // Find child which may contain key in internal node.
    pageid_t BPT::find_child(const Internal & page, const key_t & key) {
        int idx = find_lower_bound_in_internal(page, key);
        return (idx < 0) ? page.leftmost_page : page.key_ptr_pairs[idx].nxt_page;
    }

    /* Find a lower bound of key in the leaf page.
     * Returns an index of the first record in the leaf page which does not compare less than key.
     * If all keys in the leaf page compare less than key, the function returns last index (page.num_of_keys).
     */
    inline int BPT::find_lower_bound_in_leaf(const Leaf & page, const key_t & key) {
        const Record * ptr = std::lower_bound(page.records, page.records + page.num_of_keys, key,
            [](const Record & lhs, const key_t & rhs) -> bool { return lhs.key < rhs; });
        return ptr - page.records;
    }

    /* Find a lower bound of key in the internal page.
     * Returns an index of the first record in the internal page which does not compare less than key.
     * If all keys in the internal page compare less than key, the function returns last index (page.num_of_keys).
     */
    inline int BPT::find_lower_bound_in_internal(const Internal & page, const key_t & key) {
        const KeyPtr * ptr = std::lower_bound(page.key_ptr_pairs, page.key_ptr_pairs + page.num_of_keys, key,
            [](const KeyPtr & lhs, const key_t & rhs) -> bool { return lhs.key < rhs; });
        return ptr - page.key_ptr_pairs;
    }

    int BPT::_insert(const key_t & key, const value_t & value) {
        KeyPtr * key_ptr_from_root = root->is_leaf ?
            insert_into_leaf(*static_cast<Leaf *>(root), key, value) :
            insert_into_internal(*static_cast<Internal *>(root), key, value);

        // Split happened at root.
        if (key_ptr_from_root) {
            // TODO: Split root.
        }
        return 0;
    }

    /* Insert into subtree.
     * If split occurs, return a KeyPtr which need to be inserted in parent node.
     * Otherwise return nullptr.
     */
    BPT::KeyPtr * BPT::insert_into_internal(Internal & page, const key_t & key, const value_t & value) {
        pageid_t child_id = find_child(page, key);
        Node * child = get_node(child_id);
        KeyPtr * key_ptr_from_child = child->is_leaf ?
            insert_into_leaf(*static_cast<Leaf *>(child), key, value) :
            insert_into_internal(*static_cast<Internal *>(child), key, value);
        
        // Split occured in child.
        if (key_ptr_from_child) {

            if (page.num_of_keys == ORDER_INTERNAL) {
                int ret = find_lower_bound_in_internal(page, key);
                
            } else {

            }
        } else ; // no split, do nothing.

        delete child;
        return nullptr;
    }

    BPT::KeyPtr * BPT::insert_into_leaf(Leaf & page, const key_t & key, const value_t & value) {
        int idx = find_lower_bound_in_leaf(page, key);
        if (page.records[idx].key == key) {     // Key duplication
            return nullptr;
        }

        if (page.num_of_keys == ORDER_LEAF) {   // Leaf is full, split.
            Leaf & new_leaf = *reinterpret_cast<Leaf *>(disk_mgr->alloc());
            int split_idx = ORDER_LEAF >> 1;
            int is_right = idx >= split_idx;
            

        } else {
            // Shift elements.
            std::for_each(page.records + idx, page.records + page.num_of_keys, 
                [](Record & record) -> void { memcpy(&record + 1, &record, sizeof(record)); });
            page.records[idx].key   = key;
            page.records[idx].value = value;
            page.write(disk_mgr, )
            return nullptr;
        }
    }

    int BPT::_delete(const key_t & key) {
        return 0;
    }

}