#include "engine/btree.hpp"
#include "engine/cell_value.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage.hpp"
#include <algorithm>
#include <stdexcept>

namespace db {

// Row-level WAL is handled by Storage; keep physical page snapshots disabled.
static void walLogPageImage(BufferPool& pool, PageId page_id, Page& pg) {
    (void)pool;
    (void)page_id;
    (void)pg;
}

std::string BPlusTree::pack_row_blob(const Row& row) const {
    std::string s = serialize_row_disk(*row_schema_, row);
    if (s.size() > 65535u)
        throw std::runtime_error("BPlusTree: row serialized blob exceeds 65535 bytes");
    return s;
}

Row BPlusTree::unpack_row_blob(const uint8_t* blob, uint32_t blob_len) const {
    Row out;
    if (!deserialize_row_disk(*row_schema_, blob, blob_len, out))
        throw std::runtime_error("BPlusTree: failed to deserialize typed row blob");
    return out;
}

BPlusTree::BPlusTree(BufferPool& pool, PageId root_page_id, const TableSchema* row_schema,
                     uint8_t key_arity)
    : pool_(pool), root_(root_page_id), row_schema_(row_schema), key_arity_(key_arity) {
    if (root_ == 0 || root_ == INVALID_PAGE_ID)
        throw std::runtime_error("BPlusTree: invalid root_page_id (cannot be 0 or -1)");
    if (!row_schema_)
        throw std::runtime_error("BPlusTree: row_schema is required for typed persistence");
    if (key_arity_ != 1 && key_arity_ != 2)
        throw std::runtime_error("BPlusTree: key_arity must be 1 or 2");
}

void BPlusTree::validateKeyArity(const BTreeKey& k) const {
    if (k.size() != key_arity_)
        throw std::runtime_error("BPlusTree: key component count mismatch");
}

std::string BPlusTree::packKeyBlob(const BTreeKey& key) const {
    validateKeyArity(key);
    std::string s;
    btree_key_append_bytes(s, key);
    return s;
}

bool BPlusTree::unpackKeyBlob(const std::string& blob, BTreeKey& out) const {
    return decode_btree_key_blob(reinterpret_cast<const uint8_t*>(blob.data()),
                               static_cast<uint32_t>(blob.size()), key_arity_, *row_schema_, out);
}

int BPlusTree::compareBlobFull(const std::string& a, const std::string& b) const {
    BTreeKey ka, kb;
    if (!unpackKeyBlob(a, ka) || !unpackKeyBlob(b, kb))
        throw std::runtime_error("BPlusTree: corrupt key encoding on page");
    return compare_btree_keys(ka, kb);
}

int BPlusTree::compareBlobNav(const BTreeKey& probe, const std::string& blob) const {
    BTreeKey kb;
    if (!unpackKeyBlob(blob, kb))
        throw std::runtime_error("BPlusTree: corrupt key encoding on page");
    return compare_btree_keys_nav(probe, kb);
}

BTreeKey BPlusTree::bulkExtractKey(const Row& row, BTreeBulkKeySpec spec) {
    if (spec.col1 < 0) {
        int i = spec.col0;
        CellValue v = (i >= 0 && i < static_cast<int>(row.size())) ? row[static_cast<size_t>(i)]
                                                                   : std::nullopt;
        return {std::move(v)};
    }
    CellValue a = (spec.col0 >= 0 && spec.col0 < static_cast<int>(row.size()))
                      ? row[static_cast<size_t>(spec.col0)]
                      : std::nullopt;
    CellValue b = (spec.col1 >= 0 && spec.col1 < static_cast<int>(row.size()))
                      ? row[static_cast<size_t>(spec.col1)]
                      : std::nullopt;
    return {std::move(a), std::move(b)};
}

// ════════════════════════════════════════════════════════════════════════
//  Leaf cell helpers
// ════════════════════════════════════════════════════════════════════════
//
//  Cell binary format (stored at end of page, growing toward front):
//    key_len(2) | key_blob | row_len(2) | row_data
//

BPlusTree::CellView BPlusTree::readCell(const Page& pg, uint32_t idx) {
    uint16_t off = leafGetCellOffset(pg, idx);
    const char* p = pg.data + off;
    CellView cv;
    uint16_t kl;
    memcpy(&kl, p, 2);
    p += 2;
    cv.key_blob.assign(p, kl);
    p += kl;
    uint16_t rl;
    memcpy(&rl, p, 2);
    p += 2;
    cv.row_ptr = p;
    cv.row_len = rl;
    return cv;
}

uint32_t BPlusTree::cellSize(const std::string& key_blob, const std::string& row_data) {
    return 2 + static_cast<uint32_t>(key_blob.size()) + 2 + static_cast<uint32_t>(row_data.size());
}

bool BPlusTree::leafInsertCell(Page& pg, const std::string& key_blob, const std::string& row_data,
                               int pos) {
    uint32_t n = pg.getNumRecords();
    uint32_t sz = cellSize(key_blob, row_data);
    uint32_t free = leafFreeSpace(pg);

    if (free < sz + CELL_PTR_SIZE)
        return false;

    uint16_t cs = leafGetContentStart(pg);
    uint16_t new_cs = cs - static_cast<uint16_t>(sz);
    char* dst = pg.data + new_cs;

    uint16_t kl = static_cast<uint16_t>(key_blob.size());
    memcpy(dst, &kl, 2);
    dst += 2;
    memcpy(dst, key_blob.data(), kl);
    dst += kl;
    uint16_t rl = static_cast<uint16_t>(row_data.size());
    memcpy(dst, &rl, 2);
    dst += 2;
    memcpy(dst, row_data.data(), rl);

    leafSetContentStart(pg, new_cs);

    for (int i = static_cast<int>(n) - 1; i >= pos; --i)
        leafSetCellOffset(pg, i + 1, leafGetCellOffset(pg, i));
    leafSetCellOffset(pg, pos, new_cs);
    pg.setNumRecords(n + 1);
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Internal page helpers
// ════════════════════════════════════════════════════════════════════════

std::vector<BPlusTree::InternalEntry> BPlusTree::readInternalEntries(const Page& pg) {
    std::vector<InternalEntry> out;
    uint32_t n = pg.getNumRecords();
    const char* p = pg.data + INTERNAL_HEADER_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        InternalEntry e;
        uint16_t kl;
        memcpy(&kl, p, 2);
        p += 2;
        e.key_blob.assign(p, kl);
        p += kl;
        memcpy(&e.child, p, 4);
        p += 4;
        out.push_back(std::move(e));
    }
    return out;
}

void BPlusTree::writeInternalPage(Page& pg, PageId first_child, const std::vector<InternalEntry>& entries) {
    pg.setPageType(INTERNAL_PAGE);
    pg.setNumRecords(static_cast<uint32_t>(entries.size()));
    internalSetFirstChild(pg, first_child);

    char* p = pg.data + INTERNAL_HEADER_SIZE;
    for (const auto& e : entries) {
        uint16_t kl = static_cast<uint16_t>(e.key_blob.size());
        memcpy(p, &kl, 2);
        p += 2;
        memcpy(p, e.key_blob.data(), kl);
        p += kl;
        memcpy(p, &e.child, 4);
        p += 4;
    }
}

// ════════════════════════════════════════════════════════════════════════
//  Tree traversal
// ════════════════════════════════════════════════════════════════════════

PageId BPlusTree::findLeaf(const BTreeKey& key) const {
    if (root_ == 0 || root_ == INVALID_PAGE_ID)
        throw std::runtime_error("BPlusTree::findLeaf: invalid root page ID");
    PageId cur = root_;
    int depth = 0;
    while (depth < 100) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t type = pg->getPageType();
        if (type == LEAF_PAGE) {
            pool_.unpinPage(cur, false);
            return cur;
        }
        if (type != INTERNAL_PAGE) {
            pool_.unpinPage(cur, false);
            throw std::runtime_error("BPlusTree::findLeaf: reached non-tree page type " +
                                     std::to_string(type) + " at page " + std::to_string(cur));
        }
        auto entries = readInternalEntries(*pg);
        PageId next = internalGetFirstChild(*pg);
        for (const auto& e : entries) {
            if (compareBlobNav(key, e.key_blob) < 0)
                break;
            next = e.child;
        }
        pool_.unpinPage(cur, false);
        if (next == cur)
            throw std::runtime_error("BPlusTree::findLeaf: infinite loop at page " + std::to_string(cur));
        cur = next;
        ++depth;
    }
    throw std::runtime_error("BPlusTree::findLeaf: exceeded max depth");
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

std::optional<Row> BPlusTree::search(const BTreeKey& key) const {
    validateKeyArity(key);
    const std::string want = packKeyBlob(key);
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);
    uint32_t n = pg->getNumRecords();
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        if (compareBlobFull(cv.key_blob, want) == 0) {
            Row row = unpack_row_blob(reinterpret_cast<const uint8_t*>(cv.row_ptr),
                                      static_cast<uint32_t>(cv.row_len));
            pool_.unpinPage(lid, false);
            return row;
        }
    }
    pool_.unpinPage(lid, false);
    return std::nullopt;
}

std::vector<Row> BPlusTree::scanAll() const {
    std::vector<Row> result;
    PageId cur = findLeftmostLeaf();
    while (cur != INVALID_PAGE_ID) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            result.push_back(
                unpack_row_blob(reinterpret_cast<const uint8_t*>(cv.row_ptr),
                                static_cast<uint32_t>(cv.row_len)));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

std::vector<Row> BPlusTree::scanPrefix(const BTreeKey& prefix_key) const {
    if (prefix_key.size() != 1)
        throw std::runtime_error("BPlusTree::scanPrefix: expected a single-component prefix key");
    std::vector<Row> result;
    PageId lid = findLeaf(prefix_key);
    PageId cur = lid;
    bool done = false;
    while (cur != INVALID_PAGE_ID && !done) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            BTreeKey stored;
            if (!unpackKeyBlob(cv.key_blob, stored))
                throw std::runtime_error("BPlusTree::scanPrefix: bad key");
            CellValue s0 = !stored.empty() ? stored[0] : CellValue{};
            int cmp = compare_cell_values(s0, prefix_key[0]);
            if (cmp < 0)
                continue;
            if (cmp > 0) {
                done = true;
                break;
            }
            result.push_back(
                unpack_row_blob(reinterpret_cast<const uint8_t*>(cv.row_ptr),
                                static_cast<uint32_t>(cv.row_len)));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

std::vector<Row> BPlusTree::scanRange(const std::optional<BTreeKey>& low,
                                      const std::optional<BTreeKey>& high) const {
    std::vector<Row> result;
    PageId cur = low.has_value() ? findLeaf(*low) : findLeftmostLeaf();
    bool done = false;
    while (cur != INVALID_PAGE_ID && !done) {
        Page* pg = pool_.fetchPage(cur);
        uint32_t n = pg->getNumRecords();
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            if (low.has_value() || high.has_value()) {
                BTreeKey stored;
                if (!unpackKeyBlob(cv.key_blob, stored))
                    throw std::runtime_error("BPlusTree::scanRange: bad key");
                if (low.has_value() && compare_btree_keys_nav(stored, *low) < 0)
                    continue;
                if (high.has_value() && compare_btree_keys_nav(stored, *high) > 0) {
                    done = true;
                    break;
                }
            }
            result.push_back(
                unpack_row_blob(reinterpret_cast<const uint8_t*>(cv.row_ptr),
                                static_cast<uint32_t>(cv.row_len)));
        }
        PageId next = leafGetNextId(*pg);
        pool_.unpinPage(cur, false);
        cur = next;
    }
    return result;
}

bool BPlusTree::upsert(const BTreeKey& key, const Row& row) {
    std::string key_blob = packKeyBlob(key);
    std::string row_data = pack_row_blob(row);
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);

    uint32_t n = pg->getNumRecords();
    int pos = -1;
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        if (compareBlobFull(key_blob, cv.key_blob) == 0) {
            pos = static_cast<int>(i);
            break;
        }
    }

    if (pos >= 0) {
        struct KV {
            std::string kb, data;
        };
        std::vector<KV> cells;
        for (uint32_t i = 0; i < n; ++i) {
            auto cv = readCell(*pg, i);
            if (static_cast<int>(i) == pos)
                cells.push_back({key_blob, row_data});
            else
                cells.push_back({cv.key_blob, std::string(cv.row_ptr, cv.row_len)});
        }

        PageId next = leafGetNextId(*pg);
        pg->reset();
        pg->setPageType(LEAF_PAGE);
        pg->setPageId(lid);
        leafSetContentStart(*pg, PAGE_SIZE);
        leafSetNextId(*pg, next);
        for (size_t i = 0; i < cells.size(); ++i)
            leafInsertCell(*pg, cells[i].kb, cells[i].data, static_cast<int>(i));

        walLogPageImage(pool_, lid, *pg);
        pool_.unpinPage(lid, true);
        return true;
    }

    pool_.unpinPage(lid, false);
    return insert(key, row);
}

bool BPlusTree::insert(const BTreeKey& key, const Row& row) {
    std::string key_blob = packKeyBlob(key);
    std::string row_data = pack_row_blob(row);
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);

    uint32_t n = pg->getNumRecords();
    int pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        int cmp = compareBlobFull(key_blob, cv.key_blob);
        if (cmp == 0) {
            pool_.unpinPage(lid, false);
            return false;
        }
        if (cmp > 0)
            pos = i + 1;
    }

    if (leafInsertCell(*pg, key_blob, row_data, pos)) {
        walLogPageImage(pool_, lid, *pg);
        pool_.unpinPage(lid, true);
        return true;
    }

    pool_.unpinPage(lid, false);
    splitLeafAndInsert(lid, key_blob, row_data);
    return true;
}

void BPlusTree::splitLeafAndInsert(PageId leaf_id, const std::string& key_blob,
                                   const std::string& row_data) {
    Page* old_pg = pool_.fetchPage(leaf_id);

    struct KV {
        std::string kb, data;
    };
    std::vector<KV> all;
    uint32_t n = old_pg->getNumRecords();
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*old_pg, i);
        all.push_back({cv.key_blob, std::string(cv.row_ptr, cv.row_len)});
    }
    int pos = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (compareBlobFull(key_blob, all[i].kb) > 0)
            pos = static_cast<int>(i) + 1;
    }
    all.insert(all.begin() + pos, {key_blob, row_data});

    size_t mid = all.size() / 2;

    PageId old_next = leafGetNextId(*old_pg);
    old_pg->reset();
    old_pg->setPageType(LEAF_PAGE);
    old_pg->setPageId(leaf_id);
    leafSetContentStart(*old_pg, PAGE_SIZE);
    leafSetNextId(*old_pg, INVALID_PAGE_ID);

    for (size_t i = 0; i < mid; ++i)
        leafInsertCell(*old_pg, all[i].kb, all[i].data, static_cast<int>(i));

    PageId new_id;
    Page* new_pg = pool_.newPage(&new_id);
    new_pg->setPageType(LEAF_PAGE);
    new_pg->setPageId(new_id);
    leafSetContentStart(*new_pg, PAGE_SIZE);
    leafSetNextId(*new_pg, old_next);

    for (size_t i = mid; i < all.size(); ++i)
        leafInsertCell(*new_pg, all[i].kb, all[i].data, static_cast<int>(i - mid));

    leafSetNextId(*old_pg, new_id);

    std::string sep = all[mid].kb;

    walLogPageImage(pool_, leaf_id, *old_pg);
    walLogPageImage(pool_, new_id, *new_pg);
    pool_.unpinPage(leaf_id, true);
    pool_.unpinPage(new_id, true);

    insertIntoParent(leaf_id, sep, new_id);
}

void BPlusTree::insertIntoParent(PageId left_id, const std::string& key_blob, PageId right_id) {
    if (left_id == root_) {
        PageId new_root_id;
        Page* rp = pool_.newPage(&new_root_id);
        rp->setPageType(INTERNAL_PAGE);
        rp->setPageId(new_root_id);
        std::vector<InternalEntry> entries = {{key_blob, right_id}};
        writeInternalPage(*rp, left_id, entries);
        walLogPageImage(pool_, new_root_id, *rp);
        pool_.unpinPage(new_root_id, true);
        root_ = new_root_id;
        return;
    }

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

        bool found = (fc == left_id);
        for (const auto& e : entries) {
            if (e.child == left_id) {
                found = true;
                break;
            }
        }
        if (found) {
            parent_id = cur;
            pool_.unpinPage(cur, false);
            break;
        }

        BTreeKey sep_key;
        if (!unpackKeyBlob(key_blob, sep_key))
            throw std::runtime_error("BPlusTree::insertIntoParent: bad separator key");
        PageId next = fc;
        for (const auto& e : entries) {
            if (compareBlobNav(sep_key, e.key_blob) < 0)
                break;
            next = e.child;
        }
        pool_.unpinPage(cur, false);
        cur = next;
    }

    if (parent_id == INVALID_PAGE_ID) {
        PageId nr;
        Page* rp = pool_.newPage(&nr);
        rp->setPageType(INTERNAL_PAGE);
        rp->setPageId(nr);
        writeInternalPage(*rp, left_id, {{key_blob, right_id}});
        pool_.unpinPage(nr, true);
        root_ = nr;
        return;
    }

    Page* pp = pool_.fetchPage(parent_id);
    auto entries = readInternalEntries(*pp);

    BTreeKey sep_key;
    if (!unpackKeyBlob(key_blob, sep_key))
        throw std::runtime_error("BPlusTree::insertIntoParent: bad separator key");

    int insert_pos = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (compareBlobNav(sep_key, entries[i].key_blob) > 0)
            insert_pos = static_cast<int>(i) + 1;
    }
    entries.insert(entries.begin() + insert_pos, {key_blob, right_id});

    uint32_t needed = INTERNAL_HEADER_SIZE;
    for (const auto& e : entries)
        needed += 6 + static_cast<uint32_t>(e.key_blob.size());

    if (needed <= PAGE_SIZE) {
        PageId fc = internalGetFirstChild(*pp);
        writeInternalPage(*pp, fc, entries);
        walLogPageImage(pool_, parent_id, *pp);
        pool_.unpinPage(parent_id, true);
    } else {
        PageId fc = internalGetFirstChild(*pp);
        pool_.unpinPage(parent_id, false);
        splitInternalAndInsert(parent_id, key_blob, right_id);
    }
}

void BPlusTree::splitInternalAndInsert(PageId node_id, const std::string& key_blob,
                                       PageId new_child_id) {
    Page* pg = pool_.fetchPage(node_id);
    auto entries = readInternalEntries(*pg);
    PageId fc = internalGetFirstChild(*pg);

    BTreeKey sep_key;
    if (!unpackKeyBlob(key_blob, sep_key))
        throw std::runtime_error("BPlusTree::splitInternalAndInsert: bad key");

    int pos = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (compareBlobNav(sep_key, entries[i].key_blob) > 0)
            pos = static_cast<int>(i) + 1;
    }
    entries.insert(entries.begin() + pos, {key_blob, new_child_id});

    size_t mid = entries.size() / 2;
    std::string push_up_key = entries[mid].key_blob;

    std::vector<InternalEntry> left_entries(entries.begin(), entries.begin() + mid);
    PageId right_fc = entries[mid].child;
    std::vector<InternalEntry> right_entries(entries.begin() + mid + 1, entries.end());

    pg->reset();
    pg->setPageId(node_id);
    writeInternalPage(*pg, fc, left_entries);
    walLogPageImage(pool_, node_id, *pg);
    pool_.unpinPage(node_id, true);

    PageId right_id;
    Page* rp = pool_.newPage(&right_id);
    rp->setPageId(right_id);
    writeInternalPage(*rp, right_fc, right_entries);
    walLogPageImage(pool_, right_id, *rp);
    pool_.unpinPage(right_id, true);

    insertIntoParent(node_id, push_up_key, right_id);
}

bool BPlusTree::remove(const BTreeKey& key) {
    std::string key_blob = packKeyBlob(key);
    PageId lid = findLeaf(key);
    Page* pg = pool_.fetchPage(lid);
    uint32_t n = pg->getNumRecords();

    int found = -1;
    for (uint32_t i = 0; i < n; ++i) {
        auto cv = readCell(*pg, i);
        if (compareBlobFull(cv.key_blob, key_blob) == 0) {
            found = static_cast<int>(i);
            break;
        }
    }
    if (found < 0) {
        pool_.unpinPage(lid, false);
        return false;
    }

    struct KV {
        std::string kb, data;
    };
    std::vector<KV> kept;
    for (uint32_t i = 0; i < n; ++i) {
        if (static_cast<int>(i) == found)
            continue;
        auto cv = readCell(*pg, i);
        kept.push_back({cv.key_blob, std::string(cv.row_ptr, cv.row_len)});
    }

    PageId next = leafGetNextId(*pg);
    pg->reset();
    pg->setPageType(LEAF_PAGE);
    pg->setPageId(lid);
    leafSetContentStart(*pg, PAGE_SIZE);
    leafSetNextId(*pg, next);

    for (size_t i = 0; i < kept.size(); ++i)
        leafInsertCell(*pg, kept[i].kb, kept[i].data, static_cast<int>(i));

    walLogPageImage(pool_, lid, *pg);
    pool_.unpinPage(lid, true);
    return true;
}

PageId BPlusTree::bulkLoad(BufferPool& pool, const std::vector<Row>& sorted_rows,
                          const TableSchema* row_schema, BTreeBulkKeySpec key_spec) {
    if (!row_schema)
        throw std::runtime_error("BPlusTree::bulkLoad: row_schema required");

    const uint8_t arity = key_spec.col1 < 0 ? 1 : 2;

    auto pack_row_key = [&](const Row& row) -> std::string {
        BTreeKey k = bulkExtractKey(row, key_spec);
        if (k.size() != arity)
            throw std::runtime_error("BPlusTree::bulkLoad: row missing key columns");
        std::string s;
        btree_key_append_bytes(s, k);
        return s;
    };

    if (sorted_rows.empty()) {
        PageId id;
        Page* pg = pool.newPage(&id);
        pg->setPageType(LEAF_PAGE);
        pg->setPageId(id);
        leafSetContentStart(*pg, PAGE_SIZE);
        leafSetNextId(*pg, INVALID_PAGE_ID);
        pool.unpinPage(id, true);
        return id;
    }

    struct LeafInfo {
        PageId      id;
        std::string first_key_blob;
    };
    std::vector<LeafInfo> leaves;

    PageId cur_id;
    Page* cur_pg = pool.newPage(&cur_id);
    cur_pg->setPageType(LEAF_PAGE);
    cur_pg->setPageId(cur_id);
    leafSetContentStart(*cur_pg, PAGE_SIZE);
    leafSetNextId(*cur_pg, INVALID_PAGE_ID);
    std::string first_key_of_leaf = pack_row_key(sorted_rows.front());

    for (size_t r = 0; r < sorted_rows.size(); ++r) {
        const Row& row = sorted_rows[r];
        std::string kb = pack_row_key(row);
        std::string rd = serialize_row_disk(*row_schema, row);
        int pos = static_cast<int>(cur_pg->getNumRecords());

        if (!leafInsertCell(*cur_pg, kb, rd, pos)) {
            leaves.push_back({cur_id, first_key_of_leaf});
            PageId new_id;
            Page* new_pg = pool.newPage(&new_id);
            new_pg->setPageType(LEAF_PAGE);
            new_pg->setPageId(new_id);
            leafSetContentStart(*new_pg, PAGE_SIZE);
            leafSetNextId(*new_pg, INVALID_PAGE_ID);

            leafSetNextId(*cur_pg, new_id);
            pool.unpinPage(cur_id, true);

            cur_pg = new_pg;
            cur_id = new_id;
            first_key_of_leaf = kb;

            leafInsertCell(*cur_pg, kb, rd, 0);
        }
    }
    leaves.push_back({cur_id, first_key_of_leaf});
    pool.unpinPage(cur_id, true);

    if (leaves.size() == 1)
        return leaves[0].id;

    struct ChildInfo {
        PageId      id;
        std::string key_blob;
    };
    std::vector<ChildInfo> level;
    for (auto& li : leaves)
        level.push_back({li.id, li.first_key_blob});

    while (level.size() > 1) {
        std::vector<ChildInfo> next_level;

        size_t i = 0;
        while (i < level.size()) {
            PageId node_id;
            Page* np = pool.newPage(&node_id);
            np->setPageType(INTERNAL_PAGE);
            np->setPageId(node_id);

            PageId first_child = level[i].id;
            std::vector<InternalEntry> entries;

            ++i;
            uint32_t space = INTERNAL_HEADER_SIZE;
            while (i < level.size()) {
                uint32_t entry_size = 6 + static_cast<uint32_t>(level[i].key_blob.size());
                if (space + entry_size > PAGE_SIZE)
                    break;
                entries.push_back({level[i].key_blob, level[i].id});
                space += entry_size;
                ++i;
            }

            writeInternalPage(*np, first_child, entries);
            pool.unpinPage(node_id, true);
            next_level.push_back({node_id, level[i - entries.size() - 1].key_blob});
        }
        level = std::move(next_level);
    }

    return level[0].id;
}

} // namespace db
