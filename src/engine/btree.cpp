#include "engine/btree.hpp"
#include <algorithm>
#include <stdexcept>
#include <cmath>

namespace db {

static void walLogPageImage(BufferPool& pool, PageId page_id, Page& pg) {
    WALManager* wal = pool.getWALManager();
    if (!wal) return;
    // Payload format:
    //   file_path_len (2) | file_path bytes | page bytes (PAGE_SIZE)
    // (file_path is needed because page_id is per-file, not global)
    const std::string& fp = pool.filePath();
    if (fp.size() > 0xFFFF) return;
    uint16_t fpl = static_cast<uint16_t>(fp.size());
    std::string payload;
    payload.reserve(2 + fp.size() + PAGE_SIZE);
    payload.append(reinterpret_cast<const char*>(&fpl), 2);
    payload.append(fp.data(), fp.size());
    payload.append(pg.data, PAGE_SIZE);

    // Payload can contain '\0' bytes; std::string is fine as a byte buffer.
    LogRecord rec(0, pg.getLSN(), LogRecordType::PAGE_IMAGE, page_id, std::move(payload));
    LSN lsn = wal->appendRecord(rec);
    pg.setLSN(lsn);
}

// ════════════════════════════════════════════════════════════════════════
//  Constructor
// ════════════════════════════════════════════════════════════════════════

BPlusTree::BPlusTree(BufferPool& pool, PageId root_page_id,
                     const std::string& key_type)
    : pool_(pool), root_(root_page_id), key_type_(key_type) {}

// ════════════════════════════════════════════════════════════════════════
//  Key comparison (type-aware)
// ════════════════════════════════════════════════════════════════════════

int BPlusTree::compareKeys(const std::string& a, const std::string& b) const {
    size_t null_a = a.find('\0');
    size_t null_b = b.find('\0');

    std::string val_a = (null_a == std::string::npos) ? a : a.substr(0, null_a);
    std::string val_b = (null_b == std::string::npos) ? b : b.substr(0, null_b);

    int cmp = 0;
    if (key_type_ == "INT") {
        long la = 0, lb = 0;
        try { if (!val_a.empty()) la = std::stol(val_a); } catch (...) {}
        try { if (!val_b.empty()) lb = std::stol(val_b); } catch (...) {}
        cmp = (la < lb) ? -1 : (la > lb) ? 1 : 0;
    } else if (key_type_ == "FLOAT") {
        double da = 0, db = 0;
        try { if (!val_a.empty()) da = std::stod(val_a); } catch (...) {}
        try { if (!val_b.empty()) db = std::stod(val_b); } catch (...) {}
        cmp = (da < db) ? -1 : (da > db) ? 1 : 0;
    } else {
        cmp = val_a.compare(val_b);
    }

    if (cmp != 0) return cmp;

    // Primary values equal. Compare PK suffixes for secondary indexes.
    if (null_a != std::string::npos && null_b != std::string::npos) {
        return a.substr(null_a + 1).compare(b.substr(null_b + 1));
    }
    // Prefix match: "val" == "val\0pk"
    return 0;
}

// ════════════════════════════════════════════════════════════════════════
//  Leaf cell helpers
// ════════════════════════════════════════════════════════════════════════
//
//  Cell binary format (stored at end of page, growing toward front):
//    key_len(2) | key_data | row_len(2) | row_data
//

BPlusTree::CellView BPlusTree::readCell(const Page& pg, uint32_t idx) {
    uint16_t off = leafGetCellOffset(pg, idx);
    const char* p = pg.data + off;
    CellView cv;
    uint16_t kl; memcpy(&kl, p, 2); p += 2;
    cv.key.assign(p, kl); p += kl;
    uint16_t rl; memcpy(&rl, p, 2); p += 2;
    cv.row_ptr = p;
    cv.row_len = rl;
    return cv;
}

uint32_t BPlusTree::cellSize(const std::string& key,
                             const std::string& row_data) {
    return 2 + key.size() + 2 + row_data.size();
}

bool BPlusTree::leafInsertCell(Page& pg, const std::string& key,
                               const std::string& row_data, int pos) {
    uint32_t n   = pg.getNumRecords();
    uint32_t sz  = cellSize(key, row_data);
    uint32_t free = leafFreeSpace(pg);

    // Need space for cell data + one new cell pointer
    if (free < sz + CELL_PTR_SIZE) return false;

    // Write cell data at content_start - sz
    uint16_t cs = leafGetContentStart(pg);
    uint16_t new_cs = cs - static_cast<uint16_t>(sz);
    char* dst = pg.data + new_cs;

    uint16_t kl = static_cast<uint16_t>(key.size());
    memcpy(dst, &kl, 2); dst += 2;
    memcpy(dst, key.data(), kl); dst += kl;
    uint16_t rl = static_cast<uint16_t>(row_data.size());
    memcpy(dst, &rl, 2); dst += 2;
    memcpy(dst, row_data.data(), rl);

    leafSetContentStart(pg, new_cs);

    // Shift cell pointers to make room at position pos
    for (int i = static_cast<int>(n) - 1; i >= pos; --i)
        leafSetCellOffset(pg, i + 1, leafGetCellOffset(pg, i));
    leafSetCellOffset(pg, pos, new_cs);
    pg.setNumRecords(n + 1);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Internal page helpers
// ════════════════════════════════════════════════════════════════════════
//
//  After common header (16 bytes):
//    first_child(4)
//    entries[]: { key_len(2), key_data, child_id(4) } × num_records
//

std::vector<BPlusTree::InternalEntry>
BPlusTree::readInternalEntries(const Page& pg) {
    std::vector<InternalEntry> out;
    uint32_t n = pg.getNumRecords();
    const char* p = pg.data + INTERNAL_HEADER_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        InternalEntry e;
        uint16_t kl; memcpy(&kl, p, 2); p += 2;
        e.key.assign(p, kl); p += kl;
        memcpy(&e.child, p, 4); p += 4;
        out.push_back(std::move(e));
    }
    return out;
}

void BPlusTree::writeInternalPage(Page& pg, PageId first_child,
                                  const std::vector<InternalEntry>& entries) {
    pg.setPageType(INTERNAL_PAGE);
    pg.setNumRecords(static_cast<uint32_t>(entries.size()));
    internalSetFirstChild(pg, first_child);

    char* p = pg.data + INTERNAL_HEADER_SIZE;
    for (const auto& e : entries) {
        uint16_t kl = static_cast<uint16_t>(e.key.size());
        memcpy(p, &kl, 2); p += 2;
        memcpy(p, e.key.data(), kl); p += kl;
        memcpy(p, &e.child, 4); p += 4;
    }
}

// ════════════════════════════════════════════════════════════════════════
//  Tree traversal
// ════════════════════════════════════════════════════════════════════════

PageId BPlusTree::findLeaf(const std::string& key) const {
    PageId cur = root_;
    while (true) {
        Page* pg = pool_.fetchPage(cur);
        if (pg->getPageType() == LEAF_PAGE) {
            pool_.unpinPage(cur, false);
            return cur;
        }
        // Internal node — find correct child
        auto entries = readInternalEntries(*pg);
        PageId next = internalGetFirstChild(*pg);
        for (const auto& e : entries) {
            if (compareKeys(key, e.key) < 0) break;
            next = e.child;
        }
        pool_.unpinPage(cur, false);
        cur = next;
    }
}

PageId BPlusTree::findLeftmostLeaf() const {
    PageId cur = root_;
    while (true) {
        Page* pg = pool_.fetchPage(cur);
        if (pg->getPageType() == LEAF_PAGE) {
            pool_.unpinPage(cur, false);
            return cur;
        }
        PageId next = internalGetFirstChild(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
}

// ════════════════════════════════════════════════════════════════════════
//  Search
// ════════════════════════════════════════════════════════════════════════

std::optional<Row> BPlusTree::search(const std::string& key) const {
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);
    uint32_t n = pg->getNumRecords();
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        if (compareKeys(cv.key, key) == 0) {
            Row row = deserializeRow(cv.row_ptr, cv.row_len);
            pool_.unpinPage(lid, false);
            return row;
        }
    }
    pool_.unpinPage(lid, false);
    return std::nullopt;
}

// ════════════════════════════════════════════════════════════════════════
//  Scan all
// ════════════════════════════════════════════════════════════════════════

std::vector<Row> BPlusTree::scanAll() const {
    std::vector<Row> result;
    PageId cur = findLeftmostLeaf();
    while (cur != INVALID_PAGE_ID) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            result.push_back(deserializeRow(cv.row_ptr, cv.row_len));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

std::vector<Row> BPlusTree::scanPrefix(const std::string& prefix) const {
    std::vector<Row> result;
    PageId lid = findLeaf(prefix);
    PageId cur = lid;

    bool done = false;
    while (cur != INVALID_PAGE_ID && !done) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            
            // Compare only the "value" part
            size_t null_idx = cv.key.find('\0');
            std::string val_part = (null_idx == std::string::npos) ? cv.key : cv.key.substr(0, null_idx);

            int cmp = 0;
            if (key_type_ == "INT") {
                long la = 0, lb = 0;
                try { if (!val_part.empty()) la = std::stol(val_part); } catch (...) {}
                try { if (!prefix.empty()) lb = std::stol(prefix); } catch (...) {}
                cmp = (la < lb) ? -1 : (la > lb) ? 1 : 0;
            } else if (key_type_ == "FLOAT") {
                double da = 0, db = 0;
                try { if (!val_part.empty()) da = std::stod(val_part); } catch (...) {}
                try { if (!prefix.empty()) db = std::stod(prefix); } catch (...) {}
                cmp = (da < db) ? -1 : (da > db) ? 1 : 0;
            } else {
                cmp = val_part.compare(prefix);
            }

            if (cmp < 0) continue;
            if (cmp > 0) { done = true; break; }
            
            result.push_back(deserializeRow(cv.row_ptr, cv.row_len));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

std::vector<Row> BPlusTree::scanRange(const std::string* low, const std::string* high) const {
    std::vector<Row> result;
    PageId cur = (low) ? findLeaf(*low) : findLeftmostLeaf();
    
    bool done = false;
    while (cur != INVALID_PAGE_ID && !done) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            if (low && compareKeys(cv.key, *low) < 0) continue;
            if (high && compareKeys(cv.key, *high) > 0) { done = true; break; }
            result.push_back(deserializeRow(cv.row_ptr, cv.row_len));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

// ════════════════════════════════════════════════════════════════════════
//  Insert
// ════════════════════════════════════════════════════════════════════════

bool BPlusTree::insert(const std::string& key, const Row& row) {
    std::string row_data = serializeRow(row);
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);

    // Find sorted insertion position
    uint32_t n = pg->getNumRecords();
    int pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        int cmp = compareKeys(key, cv.key);
        if (cmp == 0) {
            // Duplicate key — overwrite
            // Simplest approach: fall through to split path which rebuilds
            // For now, just skip duplicate
            pool_.unpinPage(lid, false);
            return false;
        }
        if (cmp > 0) pos = i + 1;
    }

    if (leafInsertCell(*pg, key, row_data, pos)) {
        // Physical WAL: log full page after-image so splits/internal updates are recoverable too.
        walLogPageImage(pool_, lid, *pg);
        pool_.unpinPage(lid, true);
        return true;
    }

    // Leaf is full — need to split
    pool_.unpinPage(lid, false);
    splitLeafAndInsert(lid, key, row_data);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Leaf split
// ════════════════════════════════════════════════════════════════════════

void BPlusTree::splitLeafAndInsert(PageId leaf_id,
                                   const std::string& key,
                                   const std::string& row_data) {
    Page* old_pg = pool_.fetchPage(leaf_id);

    // Collect all existing cells + the new one
    struct KV { std::string key, data; };
    std::vector<KV> all;
    uint32_t n = old_pg->getNumRecords();
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*old_pg, i);
        all.push_back({cv.key,
                       std::string(cv.row_ptr, cv.row_len)});
    }
    // Insert new cell in sorted position
    int pos = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (compareKeys(key, all[i].key) > 0) pos = i + 1;
    }
    all.insert(all.begin() + pos, {key, row_data});

    // Split: first half stays, second half goes to new leaf
    size_t mid = all.size() / 2;

    // Reinitialize old leaf
    PageId old_next = leafGetNextId(*old_pg);
    old_pg->reset();
    old_pg->setPageType(LEAF_PAGE);
    old_pg->setPageId(leaf_id);
    leafSetContentStart(*old_pg, PAGE_SIZE);
    leafSetNextId(*old_pg, INVALID_PAGE_ID); // will set below

    for (size_t i = 0; i < mid; ++i)
        leafInsertCell(*old_pg, all[i].key, all[i].data,
                       static_cast<int>(i));

    // Create new leaf
    PageId new_id;
    Page* new_pg = pool_.newPage(&new_id);
    new_pg->setPageType(LEAF_PAGE);
    new_pg->setPageId(new_id);
    leafSetContentStart(*new_pg, PAGE_SIZE);
    leafSetNextId(*new_pg, old_next);

    for (size_t i = mid; i < all.size(); ++i)
        leafInsertCell(*new_pg, all[i].key, all[i].data,
                       static_cast<int>(i - mid));

    // Link old → new
    leafSetNextId(*old_pg, new_id);

    // Separator key = first key of new leaf
    std::string sep = all[mid].key;

    walLogPageImage(pool_, leaf_id, *old_pg);
    walLogPageImage(pool_, new_id, *new_pg);
    pool_.unpinPage(leaf_id, true);
    pool_.unpinPage(new_id, true);

    insertIntoParent(leaf_id, sep, new_id);
}

// ════════════════════════════════════════════════════════════════════════
//  Insert into parent (may cascade splits upward)
// ════════════════════════════════════════════════════════════════════════

void BPlusTree::insertIntoParent(PageId left_id, const std::string& key,
                                 PageId right_id) {
    // If left is the root, create a new root
    if (left_id == root_) {
        PageId new_root_id;
        Page* rp = pool_.newPage(&new_root_id);
        rp->setPageType(INTERNAL_PAGE);
        rp->setPageId(new_root_id);
        std::vector<InternalEntry> entries = {{key, right_id}};
        writeInternalPage(*rp, left_id, entries);
        walLogPageImage(pool_, new_root_id, *rp);
        pool_.unpinPage(new_root_id, true);
        root_ = new_root_id;
        return;
    }

    // Find parent by searching from root
    // (Simple approach: walk down from root tracking parent)
    PageId parent_id = INVALID_PAGE_ID;
    PageId cur = root_;
    while (true) {
        Page* pg = pool_.fetchPage(cur);
        if (pg->getPageType() == LEAF_PAGE) {
            pool_.unpinPage(cur, false);
            break;
        }
        auto entries = readInternalEntries(*pg);
        PageId fc = internalGetFirstChild(*pg);

        // Check if any child is left_id
        bool found = false;
        if (fc == left_id) { found = true; }
        for (const auto& e : entries) {
            if (e.child == left_id) { found = true; break; }
        }
        if (found) {
            parent_id = cur;
            pool_.unpinPage(cur, false);
            break;
        }

        // Descend
        PageId next = fc;
        // Use the key to find the right path
        for (const auto& e : entries) {
            if (compareKeys(key, e.key) < 0) break;
            next = e.child;
        }
        pool_.unpinPage(cur, false);
        cur = next;
    }

    if (parent_id == INVALID_PAGE_ID) {
        // Shouldn't happen, but create new root as fallback
        PageId nr;
        Page* rp = pool_.newPage(&nr);
        rp->setPageType(INTERNAL_PAGE);
        rp->setPageId(nr);
        writeInternalPage(*rp, left_id, {{key, right_id}});
        pool_.unpinPage(nr, true);
        root_ = nr;
        return;
    }

    // Try to insert into existing parent
    Page* pp = pool_.fetchPage(parent_id);
    auto entries = readInternalEntries(*pp);

    // Find position for new key
    int insert_pos = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (compareKeys(key, entries[i].key) > 0)
            insert_pos = i + 1;
    }
    entries.insert(entries.begin() + insert_pos, {key, right_id});

    // Check if it still fits
    uint32_t needed = INTERNAL_HEADER_SIZE;
    for (const auto& e : entries)
        needed += 2 + e.key.size() + 4;

    if (needed <= PAGE_SIZE) {
        PageId fc = internalGetFirstChild(*pp);
        writeInternalPage(*pp, fc, entries);
        walLogPageImage(pool_, parent_id, *pp);
        pool_.unpinPage(parent_id, true);
    } else {
        PageId fc = internalGetFirstChild(*pp);
        pool_.unpinPage(parent_id, false);
        splitInternalAndInsert(parent_id, key, right_id);
    }
}

// ════════════════════════════════════════════════════════════════════════
//  Internal node split
// ════════════════════════════════════════════════════════════════════════

void BPlusTree::splitInternalAndInsert(PageId node_id,
                                       const std::string& key,
                                       PageId new_child_id) {
    Page* pg = pool_.fetchPage(node_id);
    auto entries = readInternalEntries(*pg);
    PageId fc = internalGetFirstChild(*pg);

    // Insert new entry in sorted position
    int pos = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (compareKeys(key, entries[i].key) > 0) pos = i + 1;
    }
    entries.insert(entries.begin() + pos, {key, new_child_id});

    // Split: push middle key up
    size_t mid = entries.size() / 2;
    std::string push_up_key = entries[mid].key;

    // Left node gets entries[0..mid-1], first_child = fc
    std::vector<InternalEntry> left_entries(entries.begin(),
                                            entries.begin() + mid);
    // Right node gets entries[mid+1..], first_child = entries[mid].child
    PageId right_fc = entries[mid].child;
    std::vector<InternalEntry> right_entries(entries.begin() + mid + 1,
                                             entries.end());

    // Rewrite left (old node)
    pg->reset();
    pg->setPageId(node_id);
    writeInternalPage(*pg, fc, left_entries);
    walLogPageImage(pool_, node_id, *pg);
    pool_.unpinPage(node_id, true);

    // Create right node
    PageId right_id;
    Page* rp = pool_.newPage(&right_id);
    rp->setPageId(right_id);
    writeInternalPage(*rp, right_fc, right_entries);
    walLogPageImage(pool_, right_id, *rp);
    pool_.unpinPage(right_id, true);

    insertIntoParent(node_id, push_up_key, right_id);
}

// ════════════════════════════════════════════════════════════════════════
//  Remove (lazy — doesn't merge/redistribute underflowing nodes)
// ════════════════════════════════════════════════════════════════════════

bool BPlusTree::remove(const std::string& key) {
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);
    uint32_t n = pg->getNumRecords();

    int found = -1;
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        if (compareKeys(cv.key, key) == 0) { found = i; break; }
    }
    if (found < 0) {
        pool_.unpinPage(lid, false);
        return false;
    }

    // Rebuild leaf without the deleted cell
    struct KV { std::string key, data; };
    std::vector<KV> kept;
    for (uint32_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == found) continue;
        auto cv = readCell(*pg, i);
        kept.push_back({cv.key,
                        std::string(cv.row_ptr, cv.row_len)});
    }

    PageId next = leafGetNextId(*pg);
    pg->reset();
    pg->setPageType(LEAF_PAGE);
    pg->setPageId(lid);
    leafSetContentStart(*pg, PAGE_SIZE);
    leafSetNextId(*pg, next);

    for (size_t i = 0; i < kept.size(); ++i)
        leafInsertCell(*pg, kept[i].key, kept[i].data,
                       static_cast<int>(i));

    walLogPageImage(pool_, lid, *pg);

    pool_.unpinPage(lid, true);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Bulk Load — bottom-up construction from sorted rows
// ════════════════════════════════════════════════════════════════════════

PageId BPlusTree::bulkLoad(BufferPool& pool,
                           const std::vector<Row>& sorted_rows,
                           uint32_t key_col,
                           const std::string& key_type) {
    if (sorted_rows.empty()) {
        // Create a single empty leaf as root
        PageId id;
        Page* pg = pool.newPage(&id);
        pg->setPageType(LEAF_PAGE);
        pg->setPageId(id);
        leafSetContentStart(*pg, PAGE_SIZE);
        leafSetNextId(*pg, INVALID_PAGE_ID);
        pool.unpinPage(id, true);
        return id;
    }

    // ── Step 1: Pack rows into leaf pages ──────────────────────────────
    struct LeafInfo { PageId id; std::string first_key; };
    std::vector<LeafInfo> leaves;

    PageId cur_id;
    Page* cur_pg = pool.newPage(&cur_id);
    cur_pg->setPageType(LEAF_PAGE);
    cur_pg->setPageId(cur_id);
    leafSetContentStart(*cur_pg, PAGE_SIZE);
    leafSetNextId(*cur_pg, INVALID_PAGE_ID);
    std::string first_key_of_leaf = sorted_rows[0][key_col];

    for (size_t r = 0; r < sorted_rows.size(); ++r) {
        const Row& row = sorted_rows[r];
        std::string key = row[key_col];
        std::string rd  = serializeRow(row);
        int pos = static_cast<int>(cur_pg->getNumRecords());

        if (!leafInsertCell(*cur_pg, key, rd, pos)) {
            // Page full — finalize and start new leaf
            leaves.push_back({cur_id, first_key_of_leaf});
            PageId new_id;
            Page* new_pg = pool.newPage(&new_id);
            new_pg->setPageType(LEAF_PAGE);
            new_pg->setPageId(new_id);
            leafSetContentStart(*new_pg, PAGE_SIZE);
            leafSetNextId(*new_pg, INVALID_PAGE_ID);

            // Link previous → current
            leafSetNextId(*cur_pg, new_id);
            pool.unpinPage(cur_id, true);

            cur_pg = new_pg;
            cur_id = new_id;
            first_key_of_leaf = key;

            leafInsertCell(*cur_pg, key, rd, 0);
        }
    }
    leaves.push_back({cur_id, first_key_of_leaf});
    pool.unpinPage(cur_id, true);

    if (leaves.size() == 1)
        return leaves[0].id;

    // ── Step 2: Build internal levels bottom-up ────────────────────────
    // Current level = leaf page IDs with their first keys
    struct ChildInfo { PageId id; std::string key; };
    std::vector<ChildInfo> level;
    for (auto& li : leaves)
        level.push_back({li.id, li.first_key});

    while (level.size() > 1) {
        std::vector<ChildInfo> next_level;

        size_t i = 0;
        while (i < level.size()) {
            PageId node_id;
            Page* np = pool.newPage(&node_id);
            np->setPageType(INTERNAL_PAGE);
            np->setPageId(node_id);

            PageId first_child = level[i].id;
            std::vector<BPlusTree::InternalEntry> entries;

            ++i;
            uint32_t space = INTERNAL_HEADER_SIZE;
            while (i < level.size()) {
                uint32_t entry_size = 2 + level[i].key.size() + 4;
                if (space + entry_size > PAGE_SIZE) break;
                entries.push_back({level[i].key, level[i].id});
                space += entry_size;
                ++i;
            }

            writeInternalPage(*np, first_child, entries);
            pool.unpinPage(node_id, true);
            next_level.push_back({node_id, level[i - entries.size() - 1].key});
        }
        level = std::move(next_level);
    }

    return level[0].id;
}

} // namespace db
