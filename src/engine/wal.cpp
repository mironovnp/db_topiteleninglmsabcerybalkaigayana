#include "engine/wal.hpp"
#include "engine/storage/storage.hpp"
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unordered_set>
#include <unordered_map>
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

std::string LogRecord::encodeRowPayload(const std::string& abs_path,
                                       const std::string& key,
                                       const std::string& row_blob) {
    if (abs_path.size() > 0xFFFF || key.size() > 0xFFFF)
        throw std::runtime_error("LogRecord::encodeRowPayload: path/key too large");
    std::string buf;
    uint16_t pl = static_cast<uint16_t>(abs_path.size());
    uint16_t kl = static_cast<uint16_t>(key.size());
    uint32_t rl = static_cast<uint32_t>(row_blob.size());
    buf.append(reinterpret_cast<const char*>(&pl), 2);
    buf.append(abs_path.data(), abs_path.size());
    buf.append(reinterpret_cast<const char*>(&kl), 2);
    buf.append(key.data(), key.size());
    buf.append(reinterpret_cast<const char*>(&rl), 4);
    if (rl > 0) buf.append(row_blob.data(), row_blob.size());
    return buf;
}

bool LogRecord::decodeRowPayload(const std::string& payload,
                                 std::string& out_path,
                                 std::string& out_key,
                                 std::string& out_row_blob) {
    if (payload.size() < 2) return false;
    const char* p = payload.data();
    const char* end = payload.data() + payload.size();
    uint16_t pl;
    memcpy(&pl, p, 2);
    p += 2;
    if (p + pl > end) return false;
    out_path.assign(p, pl);
    p += pl;
    if (p + 2 > end) return false;
    uint16_t kl;
    memcpy(&kl, p, 2);
    p += 2;
    if (p + kl > end) return false;
    out_key.assign(p, kl);
    p += kl;
    if (p + 4 > end) return false;
    uint32_t rl;
    memcpy(&rl, p, 4);
    p += 4;
    if (rl > 100ull * 1024 * 1024) return false;
    if (p + rl > end) return false;
    out_row_blob.assign(p, rl);
    return true;
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
#ifndef _WIN32
    if (log_fd_ >= 0) {
        ::close(log_fd_);
        log_fd_ = -1;
    }
#endif
}

void WALManager::openFile() {
    auto parent = std::filesystem::path(log_file_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

#ifndef _WIN32
    // Open (or create) the WAL file once and keep an fd so we can fdatasync().
    log_fd_ = ::open(log_file_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (log_fd_ < 0) {
        throw std::runtime_error("WALManager: cannot open " + log_file_path_ + " (errno=" + std::to_string(errno) + ")");
    }

    // Determine next LSN by streaming scan (no OOM on large logs).
    off_t end = ::lseek(log_fd_, 0, SEEK_END);
    if (end <= 0) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    if (::lseek(log_fd_, 0, SEEK_SET) < 0) {
        throw std::runtime_error("WALManager: lseek failed for " + log_file_path_);
    }

    LSN last_lsn = 0;
    while (true) {
        uint32_t total_len = 0;
        ssize_t r = ::read(log_fd_, &total_len, sizeof(total_len));
        if (r == 0) break; // EOF
        if (r < 0) throw std::runtime_error("WALManager: read failed (len) for " + log_file_path_);
        if (r != static_cast<ssize_t>(sizeof(total_len))) break; // partial/truncated

        // Read type (1) + lsn (4) so we can track last_lsn without buffering the full record.
        uint8_t type = 0;
        uint32_t lsn = 0;
        r = ::read(log_fd_, &type, 1);
        if (r != 1) break;
        r = ::read(log_fd_, &lsn, 4);
        if (r != 4) break;

        last_lsn = lsn;

        // Skip the remainder of this record:
        // total_len includes: type(1) + lsn(4) + txn_id(4) + prev_lsn(4) + page_id(4) + payload_size(4) + payload
        // We've already consumed 1 + 4 bytes of that total_len.
        int64_t to_skip = static_cast<int64_t>(total_len) - (1 + 4);
        if (to_skip < 0) break;
        if (::lseek(log_fd_, to_skip, SEEK_CUR) < 0) break;
    }

    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;

    // Position for appends.
    ::lseek(log_fd_, 0, SEEK_END);
#else
    // Windows: keep previous simple behavior (no fdatasync here).
    // (If needed, can be upgraded to native handles later.)
    next_lsn_ = 1;
    flushed_lsn_ = 0;
#endif
}

LSN WALManager::appendRecord(LogRecord& record) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    record.lsn = next_lsn_++;
    std::string bytes = record.serialize();
    
    log_buffer_.append(bytes);
    
    return record.lsn;
}

void WALManager::flushTo(LSN lsn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (lsn <= flushed_lsn_) return;
    
    if (!log_buffer_.empty()) {
#ifndef _WIN32
        if (log_fd_ < 0) {
            throw std::runtime_error("WALManager: flushTo on closed fd");
        }

        const char* p = log_buffer_.data();
        size_t left = log_buffer_.size();
        while (left > 0) {
            ssize_t w = ::write(log_fd_, p, left);
            if (w < 0) throw std::runtime_error("WALManager: write failed for " + log_file_path_);
            p += static_cast<size_t>(w);
            left -= static_cast<size_t>(w);
        }

        // Durability for *this* file only (not a global sync()).
        if (::fdatasync(log_fd_) != 0) {
            throw std::runtime_error("WALManager: fdatasync failed for " + log_file_path_);
        }
#else
        // Windows fallback: no-op durability beyond process flush in this prototype.
#endif
        
        log_buffer_.clear();
        flushed_lsn_ = next_lsn_ - 1;
    }
}

void WALManager::recover(Storage* storage) {
#ifndef _WIN32
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (log_fd_ < 0) return;

    // Make sure we replay only durable WAL.
    if (::fdatasync(log_fd_) != 0) {
        throw std::runtime_error("WALManager: fdatasync failed for " + log_file_path_);
    }

    if (::lseek(log_fd_, 0, SEEK_SET) < 0) {
        throw std::runtime_error("WALManager: lseek failed for " + log_file_path_);
    }

    LSN last_lsn = 0;
    std::unordered_set<TxnId> active_txns;

    // Keep data fds open per file for speed.
    std::unordered_map<std::string, int> data_fds;

    auto getDataFd = [&](const std::string& path) -> int {
        auto it = data_fds.find(path);
        if (it != data_fds.end()) return it->second;
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) return -1;
        data_fds.emplace(path, fd);
        return fd;
    };

    auto readExact = [&](void* dst, size_t n) -> bool {
        char* p = static_cast<char*>(dst);
        size_t left = n;
        while (left > 0) {
            ssize_t rr = ::read(log_fd_, p, left);
            if (rr <= 0) return false;
            p += rr;
            left -= static_cast<size_t>(rr);
        }
        return true;
    };

    while (true) {
        uint32_t total_len = 0;
        ssize_t r = ::read(log_fd_, &total_len, sizeof(total_len));
        if (r == 0) break; // EOF
        if (r < 0) throw std::runtime_error("WALManager: read failed (len) for " + log_file_path_);
        if (r != static_cast<ssize_t>(sizeof(total_len))) break; // partial

        // Read fixed header fields.
        uint8_t type_u8 = 0;
        uint32_t lsn = 0;
        uint32_t txn_id = 0;
        uint32_t prev_lsn = 0;
        uint32_t page_id = 0;
        uint32_t payload_size = 0;

        if (!readExact(&type_u8, 1)) break;
        if (!readExact(&lsn, 4)) break;
        if (!readExact(&txn_id, 4)) break;
        if (!readExact(&prev_lsn, 4)) break;
        if (!readExact(&page_id, 4)) break;
        if (!readExact(&payload_size, 4)) break;

        last_lsn = lsn;

        LogRecordType type = static_cast<LogRecordType>(type_u8);
        if (type == LogRecordType::BEGIN_TXN) active_txns.insert(txn_id);
        else if (type == LogRecordType::COMMIT_TXN || type == LogRecordType::ABORT_TXN) active_txns.erase(txn_id);

        if (payload_size == 0) continue;

        if (type == LogRecordType::ROW_UPSERT || type == LogRecordType::ROW_DELETE ||
            type == LogRecordType::CLR_ROW_UPSERT || type == LogRecordType::CLR_ROW_DELETE) {
            std::string payload;
            payload.resize(payload_size);
            if (!readExact(payload.data(), payload.size())) break;
            if (storage) {
                std::string pth, ky, blob;
                if (LogRecord::decodeRowPayload(payload, pth, ky, blob)) {
                    LogRecordType replay_type =
                        (type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT :
                        (type == LogRecordType::CLR_ROW_DELETE) ? LogRecordType::ROW_DELETE : type;
                    storage->replayWalLogicalRecord(replay_type, std::move(pth), std::move(ky), std::move(blob));
                }
            }
            continue;
        }

        if (type != LogRecordType::PAGE_IMAGE) {
            if (::lseek(log_fd_, payload_size, SEEK_CUR) < 0) break;
            continue;
        }

        std::string payload;
        payload.resize(payload_size);
        if (!readExact(payload.data(), payload.size())) break;

        // Parse payload: file_path_len(2) | file_path | page_bytes(PAGE_SIZE)
        if (payload.size() < 2) continue;
        const char* pp = payload.data();
        uint16_t fpl = 0;
        memcpy(&fpl, pp, 2);
        pp += 2;
        if (payload.size() < 2 + fpl + PAGE_SIZE) continue;

        std::string file_path(pp, fpl);
        pp += fpl;

        int fd = getDataFd(file_path);
        if (fd < 0) continue;

        off_t off = static_cast<off_t>(page_id) * PAGE_SIZE;

        // Idempotent redo: check on-disk pageLSN.
        bool should_write = true;
        Page cur{};
        ssize_t pr = ::pread(fd, cur.data, PAGE_SIZE, off);
        if (pr == static_cast<ssize_t>(PAGE_SIZE)) {
            if (cur.getLSN() >= lsn) should_write = false;
        }

        if (!should_write) continue;

        Page pg;
        memcpy(pg.data, pp, PAGE_SIZE);
        pg.setLSN(lsn);

        off_t end = ::lseek(fd, 0, SEEK_END);
        if (end < off + static_cast<off_t>(PAGE_SIZE)) {
            if (::ftruncate(fd, off + static_cast<off_t>(PAGE_SIZE)) != 0) continue;
        }

        ssize_t pw = ::pwrite(fd, pg.data, PAGE_SIZE, off);
        if (pw != static_cast<ssize_t>(PAGE_SIZE)) {
            continue;
        }
    }

    // Sync all touched data files once.
    for (auto& [_, fd] : data_fds) {
        ::fdatasync(fd);
        ::close(fd);
    }

    // After recovery, set LSN pointers to the end of the durable log.
    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;

    if (!active_txns.empty() && storage) {
        off_t size = ::lseek(log_fd_, 0, SEEK_END);
        std::string data;
        data.resize(size);
        ::pread(log_fd_, data.data(), size, 0);

        std::vector<LogRecord> records;
        uint32_t off = 0;
        while (off < data.size()) {
            auto [rec, consumed] = LogRecord::deserialize(data.data() + off,
                                                          static_cast<uint32_t>(data.size() - off));
            if (consumed == 0) break;
            records.push_back(std::move(rec));
            off += consumed;
        }

        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            const LogRecord& rec = *it;
            if (active_txns.find(rec.txn_id) == active_txns.end()) continue;
            
            if (rec.type == LogRecordType::BEGIN_TXN) {
                LogRecord abort(rec.txn_id, 0, LogRecordType::ABORT_TXN, 0);
                abort.lsn = next_lsn_++;
                std::string bytes = abort.serialize();
                log_buffer_.append(bytes);
                active_txns.erase(rec.txn_id);
                if (active_txns.empty()) break;
                continue;
            }
            if (rec.type == LogRecordType::COMMIT_TXN || rec.type == LogRecordType::ABORT_TXN ||
                rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE) {
                continue;
            }
            if (rec.type != LogRecordType::ROW_UPSERT && rec.type != LogRecordType::ROW_DELETE) continue;

            std::string path, key, row_blob;
            if (!LogRecord::decodeRowPayload(rec.payload, path, key, row_blob)) continue;

            LogRecordType clr_type = LogRecordType::CLR_ROW_DELETE;
            std::string clr_blob;
            if (rec.type == LogRecordType::ROW_DELETE) {
                if (row_blob.empty()) continue;
                clr_type = LogRecordType::CLR_ROW_UPSERT;
                clr_blob = row_blob;
            }

            LogRecord clr(rec.txn_id, 0, clr_type, 0, LogRecord::encodeRowPayload(path, key, clr_blob));
            clr.lsn = next_lsn_++;
            std::string bytes = clr.serialize();
            log_buffer_.append(bytes);

            LogRecordType replay_type = (clr_type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT : LogRecordType::ROW_DELETE;
            storage->replayWalLogicalRecord(replay_type, path, key, clr_blob);
        }
        
        if (!log_buffer_.empty()) {
            const char* p = log_buffer_.data();
            size_t left = log_buffer_.size();
            while (left > 0) {
                ssize_t w = ::write(log_fd_, p, left);
                if (w > 0) {
                    p += static_cast<size_t>(w);
                    left -= static_cast<size_t>(w);
                }
            }
            ::fdatasync(log_fd_);
            log_buffer_.clear();
            flushed_lsn_ = next_lsn_ - 1;
        }
    }

    ::lseek(log_fd_, 0, SEEK_END);
#endif
}

std::vector<LogRecord> WALManager::readAllRecords() {
    flushTo(getNextLSN() - 1);

    std::ifstream in(log_file_path_, std::ios::binary);
    if (!in) return {};
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<LogRecord> records;
    uint32_t off = 0;
    while (off < data.size()) {
        auto [rec, consumed] = LogRecord::deserialize(data.data() + off,
                                                      static_cast<uint32_t>(data.size() - off));
        if (consumed == 0) break;
        records.push_back(std::move(rec));
        off += consumed;
    }
    return records;
}

void WALManager::reset() {
#ifndef _WIN32
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (log_fd_ < 0) return;
    if (::ftruncate(log_fd_, 0) != 0) {
        throw std::runtime_error("WALManager: ftruncate failed for " + log_file_path_);
    }
    ::lseek(log_fd_, 0, SEEK_SET);
    next_lsn_ = 1;
    flushed_lsn_ = 0;
    log_buffer_.clear();
#endif
}

uint64_t WALManager::fileSizeBytes() const {
#ifndef _WIN32
    if (log_fd_ < 0) return 0;
    struct stat st {};
    if (::fstat(log_fd_, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
#else
    return 0;
#endif
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
