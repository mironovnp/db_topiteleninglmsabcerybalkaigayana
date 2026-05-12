#pragma once
#include "engine/page.hpp"
#include <string>
#include <mutex>
#include <vector>

namespace db {

class Storage;

using LSN = uint32_t;
using TxnId = uint32_t;

constexpr LSN INVALID_LSN = 0;

enum class LogRecordType : uint8_t {
    INIT_PAGE,
    INSERT_CELL,
    DELETE_CELL,
    // Physical logging: after-image of a full page (PAGE_SIZE bytes) in payload.
    PAGE_IMAGE,
    // Logical row redo: payload via encodeRowPayload / decodeRowPayload
    ROW_UPSERT,
    ROW_DELETE,
    CLR_ROW_UPSERT,
    CLR_ROW_DELETE,
    BEGIN_TXN,
    COMMIT_TXN,
    ABORT_TXN
};

// ════════════════════════════════════════════════════════════════════════
// LogRecord - Represents a single entry in the WAL.
// Format on disk:
//   total_len (4) | type (1) | lsn (4) | txn_id (4) | prev_lsn (4) | page_id (4) | payload_size (4) | payload (var)
// ════════════════════════════════════════════════════════════════════════
struct LogRecord {
    LogRecordType type;
    LSN lsn;
    TxnId txn_id;
    LSN prev_lsn;
    PageId page_id;
    std::string payload;

    LogRecord() = default;
    
    // For INIT_PAGE
    LogRecord(TxnId txn_id, LSN prev_lsn, LogRecordType type, PageId page_id)
        : type(type), lsn(INVALID_LSN), txn_id(txn_id), prev_lsn(prev_lsn), page_id(page_id) {}

    // For INSERT_CELL / DELETE_CELL
    LogRecord(TxnId txn_id, LSN prev_lsn, LogRecordType type, PageId page_id, std::string payload)
        : type(type), lsn(INVALID_LSN), txn_id(txn_id), prev_lsn(prev_lsn), page_id(page_id), payload(std::move(payload)) {}

    // Serialize to bytes
    std::string serialize() const;
    
    // Deserialize from bytes. Returns a record and the number of bytes consumed.
    static std::pair<LogRecord, uint32_t> deserialize(const char* data, uint32_t size);

    // ROW_UPSERT / ROW_DELETE: uint16 path_len | path | uint16 key_len | key | uint32 row_len | row
    static std::string encodeRowPayload(const std::string& abs_path,
                                       const std::string& key,
                                       const std::string& row_blob = {});
    static bool decodeRowPayload(const std::string& payload,
                                 std::string& out_path,
                                 std::string& out_key,
                                 std::string& out_row_blob);
};

// ════════════════════════════════════════════════════════════════════════
// WALManager - Manages the Write-Ahead Log file.
// ════════════════════════════════════════════════════════════════════════
class WALManager {
public:
    explicit WALManager(const std::string& log_file_path);
    ~WALManager();

    // Appends a record to the log buffer and returns its assigned LSN.
    LSN appendRecord(LogRecord& record);

    // Forces the log to disk up to the specified LSN.
    void flushTo(LSN lsn);

    // Crash recovery (redo): PAGE_IMAGE + ROW_* replay when storage != nullptr.
    void recover(Storage* storage = nullptr);

    std::vector<LogRecord> readAllRecords();

    // Reset the WAL file to empty (used after a successful checkpoint).
    void reset();

    uint64_t fileSizeBytes() const;
    
    // Gets the maximum LSN that has been durably flushed to disk.
    LSN getFlushedLSN() const;
    
    // Gets the next LSN to be assigned.
    LSN getNextLSN() const;

private:
    std::string log_file_path_;
    // Use an explicit file descriptor on POSIX so we can fdatasync()
#ifndef _WIN32
    int log_fd_ = -1;
#endif
    
    LSN next_lsn_ = 1;
    LSN flushed_lsn_ = 0;
    
    std::mutex mutex_;
    
    // In-memory buffer of records waiting to be flushed.
    // For a real system we'd use a fixed-size byte buffer, but a vector of strings is simpler for CaseChamp.
    std::string log_buffer_;
    
    void openFile();
};

} // namespace db
