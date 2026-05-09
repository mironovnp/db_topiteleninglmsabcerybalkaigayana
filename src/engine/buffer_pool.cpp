#include "engine/buffer_pool.hpp"
#include <stdexcept>
#include <filesystem>

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  Construction / destruction
// ════════════════════════════════════════════════════════════════════════

BufferPool::BufferPool(const std::string& file_path, uint32_t pool_size, WALManager* wal_mgr)
    : file_path_(file_path), pool_size_(pool_size), frames_(pool_size), wal_mgr_(wal_mgr)
{
    for (uint32_t i = 0; i < pool_size_; ++i)
        free_list_.push_back(i);
    openFile();
}

BufferPool::~BufferPool() {
    flushAll();
    if (file_.is_open()) file_.close();
}

void BufferPool::openFile() {
    // Create parent dirs if needed
    auto parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);

    // Open or create file
    if (!std::filesystem::exists(file_path_)) {
        std::ofstream create(file_path_, std::ios::binary);
        create.close();
    }

    file_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open())
        throw std::runtime_error("BufferPool: cannot open " + file_path_);

    // Determine how many pages already exist
    file_.seekg(0, std::ios::end);
    auto size = file_.tellg();
    next_page_id_ = static_cast<uint32_t>(size / PAGE_SIZE);
}

// ════════════════════════════════════════════════════════════════════════
//  Disk I/O
// ════════════════════════════════════════════════════════════════════════

void BufferPool::readFromDisk(PageId id, Page& pg) {
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE);
    file_.read(pg.data, PAGE_SIZE);
    if (!file_)
        throw std::runtime_error("BufferPool: read failed for page " +
                                 std::to_string(id));
}

void BufferPool::writeToDisk(PageId id, const Page& pg) {
    // Extend file if necessary
    auto offset = static_cast<std::streamoff>(id) * PAGE_SIZE;
    file_.seekp(0, std::ios::end);
    auto end = file_.tellp();
    if (offset >= end) {
        // Fill gap with zeroes
        auto gap = offset + PAGE_SIZE - end;
        std::vector<char> zeros(gap, 0);
        file_.write(zeros.data(), gap);
    }
    file_.seekp(offset);
    file_.write(pg.data, PAGE_SIZE);
    file_.flush();
}

// ════════════════════════════════════════════════════════════════════════
//  Frame management
// ════════════════════════════════════════════════════════════════════════

uint32_t BufferPool::getFrame() {
    // 1) Try free list first
    if (!free_list_.empty()) {
        uint32_t idx = free_list_.front();
        free_list_.pop_front();
        return idx;
    }
    // 2) Evict from LRU (front = least recently used)
    if (lru_list_.empty())
        throw std::runtime_error("BufferPool: all frames pinned, cannot evict");

    uint32_t idx = lru_list_.front();
    lru_list_.pop_front();
    lru_map_.erase(idx);

    Frame& f = frames_[idx];
    // Flush dirty page before evicting
    if (f.dirty) {
        // STEAL + WAL: before writing a dirty page, the log must be forced
        // at least up to the page's LSN.
        if (wal_mgr_) wal_mgr_->flushTo(f.page.getLSN());
        writeToDisk(f.page_id, f.page);
        f.dirty = false;
    }
    page_table_.erase(f.page_id);
    f.in_use = false;
    return idx;
}

// ════════════════════════════════════════════════════════════════════════
//  Public API
// ════════════════════════════════════════════════════════════════════════

Page* BufferPool::fetchPage(PageId page_id) {
    // Already in pool?
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Frame& f = frames_[it->second];
        if (f.pin_count == 0) {
            // Remove from LRU (no longer evictable)
            auto lru_it = lru_map_.find(it->second);
            if (lru_it != lru_map_.end()) {
                lru_list_.erase(lru_it->second);
                lru_map_.erase(lru_it);
            }
        }
        f.pin_count++;
        return &f.page;
    }

    // Cache miss — bring from disk
    uint32_t idx = getFrame();
    Frame& f = frames_[idx];
    f.page.reset();
    readFromDisk(page_id, f.page);
    f.page_id   = page_id;
    f.pin_count = 1;
    f.dirty     = false;
    f.in_use    = true;
    page_table_[page_id] = idx;
    return &f.page;
}

Page* BufferPool::newPage(PageId* out_id) {
    PageId id = next_page_id_++;
    *out_id = id;

    uint32_t idx = getFrame();
    Frame& f = frames_[idx];
    f.page.reset();
    f.page.setPageId(id);
    f.page_id   = id;
    f.pin_count = 1;
    f.dirty     = true;
    f.in_use    = true;
    page_table_[id] = idx;

    if (wal_mgr_) {
        LogRecord rec(0, 0, LogRecordType::INIT_PAGE, id);
        LSN lsn = wal_mgr_->appendRecord(rec);
        f.page.setLSN(lsn);
        wal_mgr_->flushTo(lsn);
    }

    // Write an empty page to extend the file
    writeToDisk(id, f.page);
    return &f.page;
}

void BufferPool::unpinPage(PageId page_id, bool dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;

    Frame& f = frames_[it->second];
    if (f.pin_count <= 0) return;

    if (dirty) f.dirty = true;
    f.pin_count--;

    // Became unpinned → add to LRU (back = most recently used)
    if (f.pin_count == 0) {
        lru_list_.push_back(it->second);
        lru_map_[it->second] = std::prev(lru_list_.end());
    }
}

void BufferPool::flushPage(PageId page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    Frame& f = frames_[it->second];
    if (f.dirty) {
        if (wal_mgr_) wal_mgr_->flushTo(f.page.getLSN());
        writeToDisk(f.page_id, f.page);
        f.dirty = false;
    }
}

void BufferPool::flushAll() {
    for (auto& f : frames_) {
        if (f.in_use && f.dirty) {
            if (wal_mgr_) wal_mgr_->flushTo(f.page.getLSN());
            writeToDisk(f.page_id, f.page);
            f.dirty = false;
        }
    }
}

} // namespace db
