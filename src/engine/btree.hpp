#pragma once
#include "engine/buffer_pool.hpp"
#include "engine/cell_value.hpp"
#include <optional>
#include <string>
#include <vector>

namespace db {

class TableSchema;

// ════════════════════════════════════════════════════════════════════════
//  BPlusTree — typed CellValue keys (1 part = clustered PK, 2 parts = secondary)
//
//  • Data lives in leaf pages (slotted layout).
//  • Internal pages hold separator key blobs + child pointers.
//  • Leaves form a singly-linked list via next_leaf_id.
//  • Key wire format: concatenation of TBW2 cells (see row_codec.hpp).
//
// row_schema: required — leaf row blobs + legacy key coercion.
// key_arity: 1 (clustered) or 2 (secondary: column + PK).
// ════════════════════════════════════════════════════════════════════════

struct BTreeBulkKeySpec {
    int col0 = 0;
    /// If >= 0, key is { row[col0], row[col1] } (e.g. mini index rows).
    int col1 = -1;
};

class BPlusTree {
public:
    BPlusTree(BufferPool& pool, PageId& root_page_id, const TableSchema* row_schema, uint8_t key_arity);

    bool               insert(const BTreeKey& key, const Row& row);
    bool               upsert(const BTreeKey& key, const Row& row);
    bool               remove(const BTreeKey& key);
    std::optional<Row> search(const BTreeKey& key) const;

    std::vector<Row> scanAll() const;
    /// Prefix on the first key component (secondary indexes).
    std::vector<Row> scanPrefix(const BTreeKey& prefix_key) const;
    std::vector<Row> scanRange(const std::optional<BTreeKey>& low,
                               const std::optional<BTreeKey>& high) const;

    static BTreeKey bulkExtractKey(const Row& row, BTreeBulkKeySpec spec);

    static PageId bulkLoad(BufferPool& pool, const std::vector<Row>& sorted_rows,
                          const TableSchema* row_schema, BTreeBulkKeySpec key_spec);

    PageId getRootPageId() const { return root_; }

private:
    BufferPool&          pool_;
    PageId&              root_;
    const TableSchema*   row_schema_;
    uint8_t              key_arity_;

    void validateKeyArity(const BTreeKey& k) const;

    std::string packKeyBlob(const BTreeKey& key) const;
    bool        unpackKeyBlob(const std::string& blob, BTreeKey& out) const;

    int compareBlobFull(const std::string& a, const std::string& b) const;
    int compareBlobNav(const BTreeKey& probe, const std::string& blob) const;

    std::string pack_row_blob(const Row& row) const;
    Row         unpack_row_blob(const uint8_t* blob, uint32_t blob_len) const;

    struct CellView {
        std::string key_blob;
        const char* row_ptr = nullptr;
        uint16_t    row_len = 0;
    };

    static CellView  readCell(const Page& pg, uint32_t idx);
    static uint32_t cellSize(const std::string& key_blob, const std::string& row_data);
    static bool     leafInsertCell(Page& pg, const std::string& key_blob, const std::string& row_data,
                                   int pos);

    struct InternalEntry {
        std::string key_blob;
        PageId      child;
    };

    static std::vector<InternalEntry> readInternalEntries(const Page& pg);
    static void writeInternalPage(Page& pg, PageId first_child, const std::vector<InternalEntry>& entries);

    PageId findLeaf(const BTreeKey& key) const;
    PageId findLeftmostLeaf() const;

    void splitLeafAndInsert(PageId leaf_id, const std::string& key_blob, const std::string& row_data);
    void insertIntoParent(PageId left_id, const std::string& key_blob, PageId right_id);
    void splitInternalAndInsert(PageId node_id, const std::string& key_blob, PageId new_child_id);
};

} // namespace db
