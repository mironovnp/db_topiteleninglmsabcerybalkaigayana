#pragma once
#include "engine/page.hpp"
#include "engine/wal.hpp"
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <fstream>
#include <mutex>

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  BufferPool — LRU page cache between B+ tree and disk
// ════════════════════════════════════════════════════════════════════════

class BufferPool {
public:
    explicit BufferPool(const std::string& file_path,
                        uint32_t pool_size = POOL_SIZE,
                        WALManager* wal_mgr = nullptr,
                        bool replay_mode = false);
    ~BufferPool();

    Page* fetchPage(PageId page_id);
    Page* newPage(PageId* out_id);
    void unpinPage(PageId page_id, bool dirty);
    void flushPage(PageId page_id);
    void flushAll();

    uint32_t filePageCount() const { return next_page_id_; }
    WALManager* getWALManager() const { return wal_mgr_; }
    const std::string& filePath() const { return file_path_; }
    bool replayMode() const { return replay_mode_; }

private:
    std::mutex latch_; // Мьютекс для защиты внутренних структур пула

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

    std::unordered_map<PageId, uint32_t> page_table_;
    std::list<uint32_t>     lru_list_;
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> lru_map_;
    std::list<uint32_t>     free_list_;

    mutable std::fstream    file_;
    uint32_t                next_page_id_ = 0;
    WALManager* wal_mgr_ = nullptr;
    bool                    replay_mode_ = false;

    void     openFile();
    void     readFromDisk(PageId id, Page& pg);
    void     writeToDisk(PageId id, const Page& pg);
    uint32_t getFrame();
};

} // namespace db
