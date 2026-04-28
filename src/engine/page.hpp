#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace db {

// ── Constants ──────────────────────────────────────────────────────────

constexpr uint32_t PAGE_SIZE        = 8192;          // 8 KB
constexpr uint32_t POOL_SIZE        = 256;           // 256 frames = 2 MB
constexpr uint32_t INVALID_PAGE_ID  = 0xFFFFFFFF;

using PageId = uint32_t;
using Row    = std::vector<std::string>;

// ── Page types ─────────────────────────────────────────────────────────

enum PageType : uint32_t {
    META_PAGE     = 0,
    INTERNAL_PAGE = 1,
    LEAF_PAGE     = 2,
};

// ════════════════════════════════════════════════════════════════════════
//  Raw page — fixed-size byte buffer with typed header accessors
//
//  Common header (first 16 bytes):
//    [0..3]   page_type     uint32
//    [4..7]   num_records   uint32
//    [8..11]  page_id       uint32
//    [12..15] (reserved)    uint32
// ════════════════════════════════════════════════════════════════════════

struct Page {
    char data[PAGE_SIZE]{};

    uint32_t getPageType()   const { uint32_t v; memcpy(&v, data,     4); return v; }
    uint32_t getNumRecords() const { uint32_t v; memcpy(&v, data + 4, 4); return v; }
    PageId   getPageId()     const { PageId   v; memcpy(&v, data + 8, 4); return v; }

    void setPageType(uint32_t t)   { memcpy(data,     &t, 4); }
    void setNumRecords(uint32_t n) { memcpy(data + 4, &n, 4); }
    void setPageId(PageId id)      { memcpy(data + 8, &id, 4); }

    void reset() { memset(data, 0, PAGE_SIZE); }
};

// ════════════════════════════════════════════════════════════════════════
//  Leaf page layout (after 16-byte common header):
//
//    [16..19]  next_leaf_id         uint32
//    [20..21]  cell_content_start   uint16  (grows DOWN from PAGE_SIZE)
//    [22..23]  (padding)
//    [24..]    cell pointer array   num_records × uint16
//
//  Cell data area (end of page, grows toward front):
//    Each cell:
//      key_len(2) | key_data | row_len(2) | row_data
// ════════════════════════════════════════════════════════════════════════

constexpr uint32_t LEAF_HEADER_SIZE = 24;
constexpr uint32_t CELL_PTR_SIZE    = 2;

// Leaf-specific accessors (operate on raw Page)
inline PageId   leafGetNextId(const Page& p)      { PageId v;   memcpy(&v, p.data + 16, 4); return v; }
inline uint16_t leafGetContentStart(const Page& p) { uint16_t v; memcpy(&v, p.data + 20, 2); return v; }
inline void     leafSetNextId(Page& p, PageId id)  { memcpy(p.data + 16, &id, 4); }
inline void     leafSetContentStart(Page& p, uint16_t off) { memcpy(p.data + 20, &off, 2); }

inline uint16_t leafGetCellOffset(const Page& p, uint32_t idx) {
    uint16_t v; memcpy(&v, p.data + LEAF_HEADER_SIZE + idx * CELL_PTR_SIZE, 2); return v;
}
inline void leafSetCellOffset(Page& p, uint32_t idx, uint16_t off) {
    memcpy(p.data + LEAF_HEADER_SIZE + idx * CELL_PTR_SIZE, &off, 2);
}

inline uint32_t leafFreeSpace(const Page& p) {
    uint32_t dir_end = LEAF_HEADER_SIZE + p.getNumRecords() * CELL_PTR_SIZE;
    return leafGetContentStart(p) - dir_end;
}

// ════════════════════════════════════════════════════════════════════════
//  Internal page layout (after 16-byte common header):
//
//    [16..19]  first_child_id   uint32  (left-most child, before key[0])
//    [20..]    entries: { key_len(2), key_data, child_id(4) } × num_records
//             child_id is the pointer AFTER this key
// ════════════════════════════════════════════════════════════════════════

constexpr uint32_t INTERNAL_HEADER_SIZE = 20;

inline PageId internalGetFirstChild(const Page& p) {
    PageId v; memcpy(&v, p.data + 16, 4); return v;
}
inline void internalSetFirstChild(Page& p, PageId id) {
    memcpy(p.data + 16, &id, 4);
}

// ── Row serialization ──────────────────────────────────────────────────
//  Format: num_fields(2) { field_len(2) field_data } ...

inline std::string serializeRow(const Row& row) {
    std::string buf;
    uint16_t n = static_cast<uint16_t>(row.size());
    buf.append(reinterpret_cast<const char*>(&n), 2);
    for (const auto& f : row) {
        uint16_t len = static_cast<uint16_t>(f.size());
        buf.append(reinterpret_cast<const char*>(&len), 2);
        buf.append(f.data(), f.size());
    }
    return buf;
}

inline Row deserializeRow(const char* ptr, uint16_t total) {
    Row row;
    const char* end = ptr + total;
    if (ptr + 2 > end) return row;
    uint16_t n; memcpy(&n, ptr, 2); ptr += 2;
    for (uint16_t i = 0; i < n && ptr + 2 <= end; ++i) {
        uint16_t len; memcpy(&len, ptr, 2); ptr += 2;
        if (ptr + len > end) break;
        row.emplace_back(ptr, len);
        ptr += len;
    }
    return row;
}

} // namespace db
