#pragma once
#include "engine/page.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <vector>

namespace db {

using LSN = uint32_t;
using TxnId = uint32_t;

constexpr LSN INVALID_LSN = 0;

enum class LogRecordType : uint8_t {
    INIT_PAGE,
    INSERT_CELL,
    DELETE_CELL,
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
    
    // Gets the maximum LSN that has been durably flushed to disk.
    LSN getFlushedLSN() const;
    
    // Gets the next LSN to be assigned.
    LSN getNextLSN() const;

private:
    std::string log_file_path_;
    std::fstream log_file_;
    
    LSN next_lsn_ = 1;
    LSN flushed_lsn_ = 0;
    
    std::mutex mutex_;
    
    // In-memory buffer of records waiting to be flushed.
    // For a real system we'd use a fixed-size byte buffer, but a vector of strings is simpler for CaseChamp.
    std::string log_buffer_;
    
    void openFile();
};

} // namespace db
