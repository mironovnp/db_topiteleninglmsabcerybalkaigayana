#pragma once
#include "engine/page.hpp"
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <fstream>

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  BufferPool — LRU page cache between B+ tree and disk
//
//  • Holds up to POOL_SIZE frames in RAM.
//  • On miss: reads page from .db file.
//  • On eviction: writes dirty page back to disk.
//  • LRU list tracks unpinned frames; front = least recently used.
// ════════════════════════════════════════════════════════════════════════

class BufferPool {
public:
    explicit BufferPool(const std::string& file_path,
                        uint32_t pool_size = POOL_SIZE);
    ~BufferPool();

    /// Fetch existing page into pool. Returns pinned page (pin_count++).
    Page* fetchPage(PageId page_id);

    /// Allocate a brand-new page. Returns pinned page; writes *out_id.
    Page* newPage(PageId* out_id);

    /// Release pin. If dirty==true, page will be flushed on eviction.
    void unpinPage(PageId page_id, bool dirty);

    /// Force-write one page to disk.
    void flushPage(PageId page_id);

    /// Force-write ALL dirty pages to disk.
    void flushAll();

    /// Number of pages currently in the file.
    uint32_t filePageCount() const { return next_page_id_; }

private:
    struct Frame {
        Page     page;
        PageId   page_id   = INVALID_PAGE_ID;
        int      pin_count = 0;
        bool     dirty     = false;
        bool     in_use    = false;
    };

    std::string             file_path_;
    uint32_t                pool_size_;
    std::vector<Frame>      frames_;

    // PageId → frame index
    std::unordered_map<PageId, uint32_t> page_table_;

    // LRU doubly-linked list of UNPINNED frame indices
    std::list<uint32_t>     lru_list_;
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_map_;

    // Free (never-used) frame indices
    std::list<uint32_t>     free_list_;

    // Disk
    mutable std::fstream    file_;
    uint32_t                next_page_id_ = 0;

    void     openFile();
    void     readFromDisk(PageId id, Page& pg);
    void     writeToDisk(PageId id, const Page& pg);
    uint32_t getFrame();           // find free or evict LRU
};

} // namespace db
