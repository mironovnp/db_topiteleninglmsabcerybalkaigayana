#pragma once
#include "engine/buffer_pool.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  BPlusTree — clustered index on a single key column
//
//  • Data lives in leaf pages (slotted layout).
//  • Internal pages hold separator keys + child pointers.
//  • Leaves form a singly-linked list via next_leaf_id.
//  • Key comparison is type-aware (INT / FLOAT / TEXT).
// ════════════════════════════════════════════════════════════════════════

class BPlusTree {
public:
    /// key_type: "INT", "FLOAT", "TEXT", etc.  Used for comparisons.
    BPlusTree(BufferPool& pool, PageId root_page_id,
              const std::string& key_type);

    // ── Point operations ───────────────────────────────────────────────
    bool     insert(const std::string& key, const Row& row);
    bool     remove(const std::string& key);
    std::optional<Row> search(const std::string& key) const;

    // ── Scans ──────────────────────────────────────────────────────────
    std::vector<Row> scanAll() const;
    std::vector<Row> scanPrefix(const std::string& prefix) const;


    // ── Bulk load (sorted rows, key_col_index) ─────────────────────────
    /// Builds the tree bottom-up from pre-sorted rows.
    /// Returns the new root PageId.
    static PageId bulkLoad(BufferPool& pool,
                           const std::vector<Row>& sorted_rows,
                           uint32_t key_col_index,
                           const std::string& key_type);

    PageId getRootPageId() const { return root_; }

private:
    BufferPool& pool_;
    PageId      root_;
    std::string key_type_;

    // Key comparison: <0, 0, >0
    int compareKeys(const std::string& a, const std::string& b) const;

    // ── Leaf helpers ───────────────────────────────────────────────────
    struct CellView {
        std::string key;
        const char* row_ptr;
        uint16_t    row_len;
    };

    static CellView  readCell(const Page& pg, uint32_t idx);
    static uint32_t  cellSize(const std::string& key, const std::string& row_data);
    static bool      leafInsertCell(Page& pg, const std::string& key,
                                    const std::string& row_data, int pos);

    // ── Internal helpers ───────────────────────────────────────────────
    struct InternalEntry { std::string key; PageId child; };

    static std::vector<InternalEntry> readInternalEntries(const Page& pg);
    static void writeInternalPage(Page& pg, PageId first_child,
                                  const std::vector<InternalEntry>& entries);

    // ── Tree traversal ─────────────────────────────────────────────────
    PageId findLeaf(const std::string& key) const;
    PageId findLeftmostLeaf() const;

    // ── Split / insert-into-parent ─────────────────────────────────────
    void   splitLeafAndInsert(PageId leaf_id, const std::string& key,
                              const std::string& row_data);
    void   insertIntoParent(PageId left_id, const std::string& key,
                            PageId right_id);
    void   splitInternalAndInsert(PageId node_id, const std::string& key,
                                  PageId new_child_id);
};

} // namespace db
