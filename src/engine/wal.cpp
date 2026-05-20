#include "engine/wal.hpp"
#include "engine/page.hpp"
#include "engine/storage/storage.hpp"
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <fstream>

#ifdef _WIN32
// windows.h already included via wal.hpp
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unordered_set>
#include <unordered_map>
#endif

// Needed by both platforms for recover()
#include <unordered_set>
#include <unordered_map>

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
#ifdef _WIN32
    if (log_handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(log_handle_);
        log_handle_ = INVALID_HANDLE_VALUE;
    }
#else
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

#ifdef _WIN32
    // Open (or create) the WAL file and keep a HANDLE for FlushFileBuffers().
    log_handle_ = ::CreateFileA(
        log_file_path_.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,              // allow concurrent reads
        nullptr,
        OPEN_ALWAYS,                   // create if not exists
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (log_handle_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("WALManager: cannot open " + log_file_path_ +
                                 " (GetLastError=" + std::to_string(::GetLastError()) + ")");
    }

    // Determine next LSN by streaming scan.
    LARGE_INTEGER file_size;
    if (!::GetFileSizeEx(log_handle_, &file_size) || file_size.QuadPart <= 0) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    // Seek to beginning
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ::SetFilePointerEx(log_handle_, zero, nullptr, FILE_BEGIN);

    LSN last_lsn = 0;
    while (true) {
        uint32_t total_len = 0;
        DWORD bytes_read = 0;
        BOOL ok = ::ReadFile(log_handle_, &total_len, sizeof(total_len), &bytes_read, nullptr);
        if (!ok || bytes_read == 0) break; // EOF or error
        if (bytes_read != sizeof(total_len)) break; // partial/truncated

        // Read type (1) + lsn (4) so we can track last_lsn
        uint8_t type = 0;
        uint32_t lsn = 0;
        ok = ::ReadFile(log_handle_, &type, 1, &bytes_read, nullptr);
        if (!ok || bytes_read != 1) break;
        ok = ::ReadFile(log_handle_, &lsn, 4, &bytes_read, nullptr);
        if (!ok || bytes_read != 4) break;

        last_lsn = lsn;

        // Skip the remainder of this record
        int64_t to_skip = static_cast<int64_t>(total_len) - (1 + 4);
        if (to_skip < 0) break;
        LARGE_INTEGER skip_dist;
        skip_dist.QuadPart = to_skip;
        if (!::SetFilePointerEx(log_handle_, skip_dist, nullptr, FILE_CURRENT)) break;
    }

    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;

    // Position for appends.
    LARGE_INTEGER end_pos;
    end_pos.QuadPart = 0;
    ::SetFilePointerEx(log_handle_, end_pos, nullptr, FILE_END);

#else
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
#ifdef _WIN32
        if (log_handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("WALManager: flushTo on closed handle");
        }

        const char* p = log_buffer_.data();
        DWORD left = static_cast<DWORD>(log_buffer_.size());
        while (left > 0) {
            DWORD written = 0;
            if (!::WriteFile(log_handle_, p, left, &written, nullptr)) {
                throw std::runtime_error("WALManager: WriteFile failed for " + log_file_path_ +
                                         " (GetLastError=" + std::to_string(::GetLastError()) + ")");
            }
            p += written;
            left -= written;
        }

        // Durability: force data to disk (Windows equivalent of fdatasync).
        if (!::FlushFileBuffers(log_handle_)) {
            throw std::runtime_error("WALManager: FlushFileBuffers failed for " + log_file_path_);
        }
#else
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
#endif
        
        log_buffer_.clear();
        flushed_lsn_ = next_lsn_ - 1;
    }
}

// ════════════════════════════════════════════════════════════════════════
// Helper: read entire WAL file into a string (works on both platforms)
// ════════════════════════════════════════════════════════════════════════
#ifdef _WIN32
static std::string winReadEntireFile(HANDLE h) {
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0) return {};

    std::string data;
    data.resize(static_cast<size_t>(size.QuadPart));

    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);

    DWORD total_read = 0;
    char* buf = data.data();
    DWORD remaining = static_cast<DWORD>(size.QuadPart);
    while (remaining > 0) {
        DWORD bytes_read = 0;
        if (!::ReadFile(h, buf + total_read, remaining, &bytes_read, nullptr) || bytes_read == 0) break;
        total_read += bytes_read;
        remaining -= bytes_read;
    }
    data.resize(total_read);
    return data;
}

static void winWriteAll(HANDLE h, const char* p, size_t len) {
    DWORD left = static_cast<DWORD>(len);
    while (left > 0) {
        DWORD written = 0;
        if (!::WriteFile(h, p, left, &written, nullptr))
            throw std::runtime_error("WALManager: WriteFile failed during recovery");
        p += written;
        left -= written;
    }
}
#endif

void WALManager::recover(Storage* storage) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

#ifdef _WIN32
    if (log_handle_ == INVALID_HANDLE_VALUE) return;

    // 1. Read the entire durable log into memory for two-pass recovery.
    std::string log_data = winReadEntireFile(log_handle_);
    if (log_data.empty()) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    std::vector<LogRecord> records;
    uint32_t offset_p = 0;
    while (offset_p < log_data.size()) {
        auto [rec, consumed] = LogRecord::deserialize(log_data.data() + offset_p, 
                                                      static_cast<uint32_t>(log_data.size() - offset_p));
        if (consumed == 0) break;
        records.push_back(std::move(rec));
        offset_p += consumed;
    }

    if (records.empty()) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    LSN last_lsn = records.back().lsn;
    std::unordered_set<TxnId> active_txns;

    // --- PASS 1: Physical Redo (PAGE_IMAGE) ---
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::BEGIN_TXN) {
            active_txns.insert(rec.txn_id);
        } else if (rec.type == LogRecordType::COMMIT_TXN || rec.type == LogRecordType::ABORT_TXN) {
            active_txns.erase(rec.txn_id);
        }

        if (rec.type != LogRecordType::PAGE_IMAGE) continue;

        // Parse payload: file_path_len(2) | file_path | page_bytes(PAGE_SIZE)
        if (rec.payload.size() < 2) continue;
        uint16_t fpl = 0;
        memcpy(&fpl, rec.payload.data(), 2);
        if (rec.payload.size() < 2 + (size_t)fpl + PAGE_SIZE) continue;

        std::string file_path(rec.payload.data() + 2, fpl);
        const char* page_data = rec.payload.data() + 2 + fpl;

        // Open data file for writing
        auto parent_dir = std::filesystem::path(file_path).parent_path();
        if (!parent_dir.empty()) std::filesystem::create_directories(parent_dir);

        HANDLE data_h = ::CreateFileA(file_path.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ,
                                       nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (data_h == INVALID_HANDLE_VALUE) continue;

        LARGE_INTEGER off;
        off.QuadPart = static_cast<LONGLONG>(rec.page_id) * PAGE_SIZE;

        // Idempotent redo check
        Page cur_disk_page{};
        LARGE_INTEGER cur_pos;
        ::SetFilePointerEx(data_h, off, &cur_pos, FILE_BEGIN);
        DWORD bytes_read = 0;
        if (::ReadFile(data_h, cur_disk_page.data, PAGE_SIZE, &bytes_read, nullptr) &&
            bytes_read == PAGE_SIZE) {
            if (cur_disk_page.getLSN() >= rec.lsn) {
                ::CloseHandle(data_h);
                continue;
            }
        }

        Page redo_page;
        memcpy(redo_page.data, page_data, PAGE_SIZE);
        redo_page.setLSN(rec.lsn);

        // Ensure file is large enough
        LARGE_INTEGER needed_size;
        needed_size.QuadPart = off.QuadPart + PAGE_SIZE;
        LARGE_INTEGER cur_size;
        ::GetFileSizeEx(data_h, &cur_size);
        if (cur_size.QuadPart < needed_size.QuadPart) {
            ::SetFilePointerEx(data_h, needed_size, nullptr, FILE_BEGIN);
            ::SetEndOfFile(data_h);
        }

        ::SetFilePointerEx(data_h, off, nullptr, FILE_BEGIN);
        DWORD written = 0;
        ::WriteFile(data_h, redo_page.data, PAGE_SIZE, &written, nullptr);
        ::FlushFileBuffers(data_h);
        ::CloseHandle(data_h);
    }

    // --- PASS 2: Logical Redo ---
    size_t skipped_stale_logical = 0;
    if (storage) {
        for (const auto& rec : records) {
            if (rec.type == LogRecordType::ROW_UPSERT || rec.type == LogRecordType::ROW_DELETE ||
                rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE) {

                std::string pth, ky, blob;
                if (!LogRecord::decodeRowPayload(rec.payload, pth, ky, blob)) continue;
                if (!walReplayDataFileReady(pth)) {
                    ++skipped_stale_logical;
                    continue;
                }

                LogRecordType replay_type =
                    (rec.type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT :
                    (rec.type == LogRecordType::CLR_ROW_DELETE) ? LogRecordType::ROW_DELETE : rec.type;

                try {
                    storage->replayWalLogicalRecord(replay_type, pth, ky, blob);
                } catch (const std::exception& e) {
                    std::cerr << "[WAL] Logical redo failed for LSN " << rec.lsn << ": " << e.what()
                              << "\n";
                }
            }
        }
    }
    if (skipped_stale_logical > 0) {
        std::cerr << "[WAL] Skipped " << skipped_stale_logical
                  << " logical record(s) for missing or empty data files (legacy WAL entries).\n";
    }

    // --- PASS 3: Logical Undo for Uncommitted Transactions ---
    if (!active_txns.empty() && storage) {
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            const LogRecord& rec = *it;
            if (active_txns.find(rec.txn_id) == active_txns.end()) continue;
            
            if (rec.type == LogRecordType::BEGIN_TXN) {
                LogRecord abort(rec.txn_id, 0, LogRecordType::ABORT_TXN, 0);
                abort.lsn = next_lsn_++;
                log_buffer_.append(abort.serialize());
                active_txns.erase(rec.txn_id);
                if (active_txns.empty()) break;
                continue;
            }

            if (rec.type != LogRecordType::ROW_UPSERT && rec.type != LogRecordType::ROW_DELETE) continue;

            std::string path, key, row_blob;
            if (!LogRecord::decodeRowPayload(rec.payload, path, key, row_blob)) continue;
            if (!walReplayDataFileReady(path)) continue;

            LogRecordType clr_type = (rec.type == LogRecordType::ROW_DELETE) ? LogRecordType::CLR_ROW_UPSERT : LogRecordType::CLR_ROW_DELETE;
            std::string clr_blob = (rec.type == LogRecordType::ROW_DELETE) ? row_blob : "";
            if (rec.type == LogRecordType::ROW_DELETE && clr_blob.empty()) continue;

            LogRecord clr(rec.txn_id, 0, clr_type, 0, LogRecord::encodeRowPayload(path, key, clr_blob));
            clr.lsn = next_lsn_++;
            log_buffer_.append(clr.serialize());

            LogRecordType replay_type = (clr_type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT : LogRecordType::ROW_DELETE;
            try {
                storage->replayWalLogicalRecord(replay_type, path, key, clr_blob);
            } catch (...) {}
        }
    }

    // Flush CLR/ABORT records and sync data files
    if (!log_buffer_.empty()) {
        winWriteAll(log_handle_, log_buffer_.data(), log_buffer_.size());
        ::FlushFileBuffers(log_handle_);
        log_buffer_.clear();
    }

    // Compact stale entries
    if (skipped_stale_logical > 0) {
        std::vector<LogRecord> compacted;
        compacted.reserve(records.size());
        size_t dropped = 0;
        for (const auto& rec : records) {
            const bool is_logical_row =
                rec.type == LogRecordType::ROW_UPSERT || rec.type == LogRecordType::ROW_DELETE ||
                rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE;
            if (is_logical_row) {
                std::string pth, ky, blob;
                if (LogRecord::decodeRowPayload(rec.payload, pth, ky, blob) &&
                    !walReplayDataFileReady(pth)) {
                    ++dropped;
                    continue;
                }
            }
            compacted.push_back(rec);
        }
        if (dropped > 0) {
            // Truncate file
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            ::SetFilePointerEx(log_handle_, zero, nullptr, FILE_BEGIN);
            ::SetEndOfFile(log_handle_);

            log_buffer_.clear();
            LSN max_lsn = 0;
            for (const auto& rec : compacted) {
                log_buffer_.append(rec.serialize());
                max_lsn = std::max(max_lsn, rec.lsn);
            }
            if (!log_buffer_.empty()) {
                winWriteAll(log_handle_, log_buffer_.data(), log_buffer_.size());
                ::FlushFileBuffers(log_handle_);
                log_buffer_.clear();
            }
            last_lsn = max_lsn;
            std::cerr << "[WAL] Compacted log: removed " << dropped
                      << " stale logical record(s).\n";
        }
    }

    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;

    // Position for appends
    LARGE_INTEGER end_pos;
    end_pos.QuadPart = 0;
    ::SetFilePointerEx(log_handle_, end_pos, nullptr, FILE_END);

#else
    if (log_fd_ < 0) return;

    // 1. Read the entire durable log into memory for two-pass recovery.
    off_t size = ::lseek(log_fd_, 0, SEEK_END);
    if (size <= 0) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }
    
    std::string log_data;
    log_data.resize(size);
    if (::pread(log_fd_, log_data.data(), size, 0) != (ssize_t)size) {
        throw std::runtime_error("WALManager: failed to read log for recovery");
    }

    std::vector<LogRecord> records;
    uint32_t offset_p = 0;
    while (offset_p < log_data.size()) {
        auto [rec, consumed] = LogRecord::deserialize(log_data.data() + offset_p, 
                                                      static_cast<uint32_t>(log_data.size() - offset_p));
        if (consumed == 0) break;
        records.push_back(std::move(rec));
        offset_p += consumed;
    }

    if (records.empty()) {
        next_lsn_ = 1;
        flushed_lsn_ = 0;
        return;
    }

    LSN last_lsn = records.back().lsn;
    std::unordered_set<TxnId> active_txns;
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

    // --- PASS 1: Physical Redo (PAGE_IMAGE) ---
    // This ensures all metadata pages (Page 0) are restored first.
    for (const auto& rec : records) {
        if (rec.type == LogRecordType::BEGIN_TXN) {
            active_txns.insert(rec.txn_id);
        } else if (rec.type == LogRecordType::COMMIT_TXN || rec.type == LogRecordType::ABORT_TXN) {
            active_txns.erase(rec.txn_id);
        }

        if (rec.type != LogRecordType::PAGE_IMAGE) continue;

        // Parse payload: file_path_len(2) | file_path | page_bytes(PAGE_SIZE)
        if (rec.payload.size() < 2) continue;
        uint16_t fpl = 0;
        memcpy(&fpl, rec.payload.data(), 2);
        if (rec.payload.size() < 2 + (size_t)fpl + PAGE_SIZE) continue;

        std::string file_path(rec.payload.data() + 2, fpl);
        const char* page_data = rec.payload.data() + 2 + fpl;

        int fd = getDataFd(file_path);
        if (fd < 0) continue;

        off_t off = static_cast<off_t>(rec.page_id) * PAGE_SIZE;
        
        // Idempotent redo check
        Page cur_disk_page{};
        if (::pread(fd, cur_disk_page.data, PAGE_SIZE, off) == (ssize_t)PAGE_SIZE) {
            if (cur_disk_page.getLSN() >= rec.lsn) continue;
        }

        Page redo_page;
        memcpy(redo_page.data, page_data, PAGE_SIZE);
        redo_page.setLSN(rec.lsn);

        // Ensure file is large enough
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size < off + (off_t)PAGE_SIZE) {
            if (ftruncate(fd, off + PAGE_SIZE) != 0) {}
        }
        ::pwrite(fd, redo_page.data, PAGE_SIZE, off);
    }

    // --- PASS 2: Logical Redo ---
    // Now that metadata is physically present, we can safely run BTree operations.
    size_t skipped_stale_logical = 0;
    if (storage) {
        for (const auto& rec : records) {
            if (rec.type == LogRecordType::ROW_UPSERT || rec.type == LogRecordType::ROW_DELETE ||
                rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE) {

                std::string pth, ky, blob;
                if (!LogRecord::decodeRowPayload(rec.payload, pth, ky, blob)) continue;
                if (!walReplayDataFileReady(pth)) {
                    ++skipped_stale_logical;
                    continue;
                }

                LogRecordType replay_type =
                    (rec.type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT :
                    (rec.type == LogRecordType::CLR_ROW_DELETE) ? LogRecordType::ROW_DELETE : rec.type;

                try {
                    storage->replayWalLogicalRecord(replay_type, pth, ky, blob);
                } catch (const std::exception& e) {
                    std::cerr << "[WAL] Logical redo failed for LSN " << rec.lsn << ": " << e.what()
                              << "\n";
                }
            }
        }
    }
    if (skipped_stale_logical > 0) {
        std::cerr << "[WAL] Skipped " << skipped_stale_logical
                  << " logical record(s) for missing or empty data files (legacy WAL entries).\n";
    }

    // --- PASS 3: Logical Undo for Uncommitted Transactions ---
    if (!active_txns.empty() && storage) {
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            const LogRecord& rec = *it;
            if (active_txns.find(rec.txn_id) == active_txns.end()) continue;
            
            if (rec.type == LogRecordType::BEGIN_TXN) {
                LogRecord abort(rec.txn_id, 0, LogRecordType::ABORT_TXN, 0);
                abort.lsn = next_lsn_++;
                log_buffer_.append(abort.serialize());
                active_txns.erase(rec.txn_id);
                if (active_txns.empty()) break;
                continue;
            }

            if (rec.type != LogRecordType::ROW_UPSERT && rec.type != LogRecordType::ROW_DELETE) continue;

            std::string path, key, row_blob;
            if (!LogRecord::decodeRowPayload(rec.payload, path, key, row_blob)) continue;
            if (!walReplayDataFileReady(path)) continue;

            LogRecordType clr_type = (rec.type == LogRecordType::ROW_DELETE) ? LogRecordType::CLR_ROW_UPSERT : LogRecordType::CLR_ROW_DELETE;
            std::string clr_blob = (rec.type == LogRecordType::ROW_DELETE) ? row_blob : "";
            if (rec.type == LogRecordType::ROW_DELETE && clr_blob.empty()) continue;

            LogRecord clr(rec.txn_id, 0, clr_type, 0, LogRecord::encodeRowPayload(path, key, clr_blob));
            clr.lsn = next_lsn_++;
            log_buffer_.append(clr.serialize());

            LogRecordType replay_type = (clr_type == LogRecordType::CLR_ROW_UPSERT) ? LogRecordType::ROW_UPSERT : LogRecordType::ROW_DELETE;
            try {
                storage->replayWalLogicalRecord(replay_type, path, key, clr_blob);
            } catch (...) {}
        }
    }

    for (auto& [_, fd] : data_fds) {
        ::fdatasync(fd);
        ::close(fd);
    }

    if (!log_buffer_.empty()) {
        const char* p = log_buffer_.data();
        size_t left = log_buffer_.size();
        while (left > 0) {
            ssize_t w = ::write(log_fd_, p, left);
            if (w <= 0) break;
            p += w; left -= w;
        }
        ::fdatasync(log_fd_);
        log_buffer_.clear();
    }

    if (skipped_stale_logical > 0) {
        std::vector<LogRecord> compacted;
        compacted.reserve(records.size());
        size_t dropped = 0;
        for (const auto& rec : records) {
            const bool is_logical_row =
                rec.type == LogRecordType::ROW_UPSERT || rec.type == LogRecordType::ROW_DELETE ||
                rec.type == LogRecordType::CLR_ROW_UPSERT || rec.type == LogRecordType::CLR_ROW_DELETE;
            if (is_logical_row) {
                std::string pth, ky, blob;
                if (LogRecord::decodeRowPayload(rec.payload, pth, ky, blob) &&
                    !walReplayDataFileReady(pth)) {
                    ++dropped;
                    continue;
                }
            }
            compacted.push_back(rec);
        }
        if (dropped > 0) {
            if (::ftruncate(log_fd_, 0) != 0) {
                throw std::runtime_error("WALManager: ftruncate failed during compaction");
            }
            ::lseek(log_fd_, 0, SEEK_SET);
            log_buffer_.clear();
            LSN max_lsn = 0;
            for (const auto& rec : compacted) {
                log_buffer_.append(rec.serialize());
                max_lsn = std::max(max_lsn, rec.lsn);
            }
            if (!log_buffer_.empty()) {
                const char* p = log_buffer_.data();
                size_t left = log_buffer_.size();
                while (left > 0) {
                    ssize_t w = ::write(log_fd_, p, left);
                    if (w < 0) throw std::runtime_error("WALManager: write failed during compaction");
                    p += static_cast<size_t>(w);
                    left -= static_cast<size_t>(w);
                }
                ::fdatasync(log_fd_);
                log_buffer_.clear();
            }
            last_lsn = max_lsn;
            std::cerr << "[WAL] Compacted log: removed " << dropped
                      << " stale logical record(s).\n";
        }
    }

    next_lsn_ = last_lsn + 1;
    flushed_lsn_ = last_lsn;
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
#ifdef _WIN32
    if (log_handle_ == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    ::SetFilePointerEx(log_handle_, zero, nullptr, FILE_BEGIN);
    if (!::SetEndOfFile(log_handle_)) {
        throw std::runtime_error("WALManager: SetEndOfFile failed for " + log_file_path_);
    }
    next_lsn_ = 1;
    flushed_lsn_ = 0;
    log_buffer_.clear();
#else
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
#ifdef _WIN32
    if (log_handle_ == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(log_handle_, &size)) return 0;
    return static_cast<uint64_t>(size.QuadPart);
#else
    if (log_fd_ < 0) return 0;
    struct stat st {};
    if (::fstat(log_fd_, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
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
