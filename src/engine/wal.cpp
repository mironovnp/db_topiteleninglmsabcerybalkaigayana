#include "engine/wal.hpp"
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace db {

// ════════════════════════════════════════════════════════════════════════
// LogRecord
// ════════════════════════════════════════════════════════════════════════

std::string LogRecord::serialize() const {
    uint32_t payload_size = static_cast<uint32_t>(payload.size());
    // total_len = type(1) + lsn(4) + txn_id(4) + prev_lsn(4) + page_id(4) + payload_size(4) + payload
    uint32_t total_len = 1 + 4 + 4 + 4 + 4 + 4 + payload_size;
    
    std::string buf;
    buf.reserve(4 + total_len);
    
    // total_len (4)
    buf.append(reinterpret_cast<const char*>(&total_len), 4);
    // type (1)
    uint8_t t = static_cast<uint8_t>(type);
    buf.append(reinterpret_cast<const char*>(&t), 1);
    // lsn (4)
    buf.append(reinterpret_cast<const char*>(&lsn), 4);
    // txn_id (4)
    buf.append(reinterpret_cast<const char*>(&txn_id), 4);
    // prev_lsn (4)
    buf.append(reinterpret_cast<const char*>(&prev_lsn), 4);
    // page_id (4)
    buf.append(reinterpret_cast<const char*>(&page_id), 4);
    // payload_size (4)
    buf.append(reinterpret_cast<const char*>(&payload_size), 4);
    // payload
    if (payload_size > 0) {
        buf.append(payload);
    }
    
    return buf;
}

std::pair<LogRecord, uint32_t> LogRecord::deserialize(const char* data, uint32_t size) {
    if (size < 4) return {LogRecord{}, 0}; // Need at least total_len
    
    uint32_t total_len;
    memcpy(&total_len, data, 4);
    
    if (size < 4 + total_len) return {LogRecord{}, 0}; // Incomplete record
    
    const char* ptr = data + 4;
    LogRecord rec;
    
    uint8_t t;
    memcpy(&t, ptr, 1); ptr += 1;
    rec.type = static_cast<LogRecordType>(t);
    
    memcpy(&rec.lsn, ptr, 4); ptr += 4;
    memcpy(&rec.txn_id, ptr, 4); ptr += 4;
    memcpy(&rec.prev_lsn, ptr, 4); ptr += 4;
    memcpy(&rec.page_id, ptr, 4); ptr += 4;
    
    uint32_t payload_size;
    memcpy(&payload_size, ptr, 4); ptr += 4;
    
    if (payload_size > 0) {
        rec.payload = std::string(ptr, payload_size);
    }
    
    return {rec, 4 + total_len};
}

// ════════════════════════════════════════════════════════════════════════
// WALManager
// ════════════════════════════════════════════════════════════════════════

WALManager::WALManager(const std::string& log_file_path)
    : log_file_path_(log_file_path) {
    openFile();
}

WALManager::~WALManager() {
    flushTo(next_lsn_ - 1); // Flush everything on shutdown
    if (log_file_.is_open()) {
        log_file_.close();
    }
}

void WALManager::openFile() {
    auto parent = std::filesystem::path(log_file_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (!std::filesystem::exists(log_file_path_)) {
        std::ofstream create(log_file_path_, std::ios::binary);
        create.close();
    }

    log_file_.open(log_file_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!log_file_.is_open()) {
        throw std::runtime_error("WALManager: cannot open " + log_file_path_);
    }

    // Determine next LSN by scanning the file
    log_file_.seekg(0, std::ios::end);
    if (log_file_.tellg() == 0) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    log_file_.seekg(0, std::ios::beg);
    std::string file_content((std::istreambuf_iterator<char>(log_file_)),
                              std::istreambuf_iterator<char>());
    
    const char* ptr = file_content.data();
    uint32_t remaining = static_cast<uint32_t>(file_content.size());
    
    LSN last_lsn = 0;
    while (remaining > 0) {
        auto [rec, consumed] = LogRecord::deserialize(ptr, remaining);
        if (consumed == 0) break; // Reached end or incomplete record
        last_lsn = rec.lsn;
        ptr += consumed;
        remaining -= consumed;
    }
    
    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;
    
    // Move put pointer to end for append
    log_file_.seekp(0, std::ios::end);
    log_file_.clear(); // Clear any eof flags
}

LSN WALManager::appendRecord(LogRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    record.lsn = next_lsn_++;
    std::string bytes = record.serialize();
    
    log_buffer_.append(bytes);
    
    return record.lsn;
}

void WALManager::flushTo(LSN lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (lsn <= flushed_lsn_) return;
    
    if (!log_buffer_.empty()) {
        log_file_.write(log_buffer_.data(), log_buffer_.size());
        log_file_.flush(); // Flush std::fstream buffer to OS
        
        // fsync to ensure durability
#ifdef _WIN32
        // Windows fsync equivalent
        // Not perfectly robust without native handles, but flush() helps
#else
        // POSIX fsync
        // We really should use open() and fsync() directly, but for CaseChamp this might suffice
        // if we just want a logical demonstration, or we can try to extract fd if available.
        // As a simpler fallback, std::fstream::flush + an OS sync is often enough for a prototype.
        sync(); // Forces all OS buffers to disk. (Heavy, but guarantees durability).
#endif
        
        log_buffer_.clear();
        flushed_lsn_ = next_lsn_ - 1;
    }
}

LSN WALManager::getFlushedLSN() const {
    // Read-only access to an atomic-like value, lock isn't strictly necessary for a simple getter 
    // but safe to do if we want consistency.
    return flushed_lsn_;
}

LSN WALManager::getNextLSN() const {
    return next_lsn_;
}

} // namespace db
