#include "engine/buffer_pool.hpp"
#include <stdexcept>
#include <filesystem>

#ifdef _WIN32
#include <io.h>      // _fileno
#include <stdio.h>   // _fileno for fstream
#endif

namespace db {

// ════════════════════════════════════════════════════════════════════════
//  Construction / destruction
// ════════════════════════════════════════════════════════════════════════

BufferPool::BufferPool(const std::string& file_path, uint32_t pool_size, WALManager* wal_mgr,
                       bool replay_mode)
    : file_path_(file_path), pool_size_(pool_size), frames_(pool_size), wal_mgr_(wal_mgr),
      replay_mode_(replay_mode)
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
    auto parent = std::filesystem::path(file_path_).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);

    if (!std::filesystem::exists(file_path_)) {
        std::ofstream create(file_path_, std::ios::binary);
        create.close();
    }

    file_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open())
        throw std::runtime_error("BufferPool: cannot open " + file_path_);

    file_.seekg(0, std::ios::end);
    auto size = file_.tellg();
    next_page_id_ = static_cast<uint32_t>(size / PAGE_SIZE);
}

// ════════════════════════════════════════════════════════════════════════
//  Disk I/O
// ════════════════════════════════════════════════════════════════════════

void BufferPool::readFromDisk(PageId id, Page& pg) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(id) * PAGE_SIZE);
    file_.read(pg.data, PAGE_SIZE);
    if (!file_)
        throw std::runtime_error("BufferPool: read failed for page " + std::to_string(id) + " in " + file_path_);
}

void BufferPool::writeToDisk(PageId id, const Page& pg) {
    file_.clear();
    auto offset = static_cast<std::streamoff>(id) * PAGE_SIZE;
    file_.seekp(0, std::ios::end);
    auto end = file_.tellp();
    if (offset >= end) {
        auto gap = offset + PAGE_SIZE - end;
        std::vector<char> zeros(gap, 0);
        file_.write(zeros.data(), gap);
    }
    file_.seekp(offset);
    file_.write(pg.data, PAGE_SIZE);
    file_.flush();

#ifdef _WIN32
    // fstream::flush() only flushes C++ buffers to OS.
    // FlushFileBuffers forces data to durable storage (Windows fsync equivalent).
    HANDLE h = ::CreateFileA(file_path_.c_str(),
                              GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(h);
        ::CloseHandle(h);
    }
#endif
}

// ════════════════════════════════════════════════════════════════════════
//  Frame management
// ════════════════════════════════════════════════════════════════════════

uint32_t BufferPool::getFrame() {
    if (!free_list_.empty()) {
        uint32_t idx = free_list_.front();
        free_list_.pop_front();
        return idx;
    }
    if (lru_list_.empty())
        throw std::runtime_error("BufferPool: all frames pinned, cannot evict");

    uint32_t idx = lru_list_.front();
    lru_list_.pop_front();
    lru_map_.erase(idx);

    Frame& f = frames_[idx];
    if (f.dirty) {
        if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
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
    std::lock_guard<std::mutex> lock(latch_); // Блокировка

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        Frame& f = frames_[it->second];
        if (f.pin_count == 0) {
            auto lru_it = lru_map_.find(it->second);
            if (lru_it != lru_map_.end()) {
                lru_list_.erase(lru_it->second);
                lru_map_.erase(lru_it);
            }
        }
        f.pin_count++;
        return &f.page;
    }

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
    std::lock_guard<std::mutex> lock(latch_); // Блокировка

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

    if (wal_mgr_ && !replay_mode_) {
        LogRecord rec(0, 0, LogRecordType::INIT_PAGE, id);
        LSN lsn = wal_mgr_->appendRecord(rec);
        f.page.setLSN(lsn);
        wal_mgr_->flushTo(lsn);
    }

    writeToDisk(id, f.page);
    return &f.page;
}

void BufferPool::unpinPage(PageId page_id, bool dirty) {
    std::lock_guard<std::mutex> lock(latch_); // Блокировка

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;

    Frame& f = frames_[it->second];
    if (f.pin_count <= 0) return;

    if (dirty) f.dirty = true;
    f.pin_count--;

    if (f.pin_count == 0) {
        lru_list_.push_back(it->second);
        lru_map_[it->second] = std::prev(lru_list_.end());
    }
}

void BufferPool::flushPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_); // Блокировка

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;
    Frame& f = frames_[it->second];
    if (f.dirty) {
        if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
        writeToDisk(f.page_id, f.page);
        f.dirty = false;
    }
}

void BufferPool::flushAll() {
    std::lock_guard<std::mutex> lock(latch_); // Блокировка

    for (auto& f : frames_) {
        if (f.in_use && f.dirty) {
            if (wal_mgr_) wal_mgr_->flushTo(wal_mgr_->getNextLSN() - 1);
            writeToDisk(f.page_id, f.page);
            f.dirty = false;
        }
    }
}

} // namespace db
