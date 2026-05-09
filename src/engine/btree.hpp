#pragma once
#include "engine/buffer_pool.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace db {

class TableSchema;

// ════════════════════════════════════════════════════════════════════════
//  BPlusTree — clustered index on a single key column
//
//  • Data lives in leaf pages (slotted layout).
//  • Internal pages hold separator keys + child pointers.
//  • Leaves form a singly-linked list via next_leaf_id.
//  • Key comparison is type-aware (INT / FLOAT / TEXT).
//
// row_schema: required — used to serialize/deserialize typed Row payloads (TBW2).
// ════════════════════════════════════════════════════════════════════════

class BPlusTree {
public:
    /// key_type: "INT", "FLOAT", "TEXT", … for key ordering of stored key strings / composite prefixes.
    BPlusTree(BufferPool& pool, PageId root_page_id, const std::string& key_type,
              const TableSchema* row_schema);

    bool     insert(const std::string& key, const Row& row);
    bool     upsert(const std::string& key, const Row& row);
    bool     remove(const std::string& key);
    std::optional<Row> search(const std::string& key) const;

    std::vector<Row> scanAll() const;
    std::vector<Row> scanPrefix(const std::string& prefix) const;
    std::vector<Row> scanRange(const std::string* low, const std::string* high) const;

    static PageId bulkLoad(BufferPool& pool, const std::vector<Row>& sorted_rows,
                          uint32_t key_col_index, const std::string& key_type,
                          const TableSchema* row_schema);

    PageId getRootPageId() const { return root_; }

private:
    BufferPool&       pool_;
    PageId            root_;
    std::string       key_type_;
    const TableSchema* row_schema_; // not null — binary row blobs

    int compareKeys(const std::string& a, const std::string& b) const;

    std::string pack_row_blob(const Row& row) const;

    Row unpack_row_blob(const uint8_t* blob, uint32_t blob_len) const;

    std::string row_key_lexical(size_t key_col_idx, const Row& row) const;

    struct CellView {
        std::string key;
        const char* row_ptr = nullptr;
        uint16_t    row_len = 0;
    };

    static CellView  readCell(const Page& pg, uint32_t idx);
    static uint32_t cellSize(const std::string& key, const std::string& row_data);
    static bool     leafInsertCell(Page& pg, const std::string& key,
                                   const std::string& row_data, int pos);

    struct InternalEntry {
        std::string key;
        PageId child;
    };

    static std::vector<InternalEntry> readInternalEntries(const Page& pg);
    static void writeInternalPage(Page& pg, PageId first_child,
                                const std::vector<InternalEntry>& entries);

    PageId findLeaf(const std::string& key) const;
    PageId findLeftmostLeaf() const;

    void splitLeafAndInsert(PageId leaf_id, const std::string& key, const std::string& row_data);
    void insertIntoParent(PageId left_id, const std::string& key, PageId right_id);
    void splitInternalAndInsert(PageId node_id, const std::string& key, PageId new_child_id);
};

} // namespace db
