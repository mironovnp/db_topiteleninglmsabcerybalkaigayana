#include "engine/storage/storage.hpp"
#include <iostream>

namespace db {

Storage::Storage(const std::string& data_dir) : data_dir_(data_dir) {
    std::filesystem::create_directories(data_dir_);
    wal_mgr_ = std::make_unique<WALManager>((data_dir_ / "wal.log").string());
    try {
        wal_mgr_->recover(this);
    } catch (const std::exception& e) {
        std::cerr << "[WAL] Recovery failed: " << e.what() << ". Some data may be lost." << std::endl;
    }
}

std::filesystem::path Storage::databaseDirectory(const std::string& db_name) const {
    return data_dir_ / db_name;
}

std::filesystem::path Storage::dbPath(const std::string& db) const {
    return data_dir_ / db;
}

std::filesystem::path Storage::tablePath(const std::string& db, const std::string& tbl) const {
    return data_dir_ / db / (tbl + ".db");
}

std::filesystem::path Storage::indexPath(const std::string& db, const std::string& tbl,
                                           const std::string& col) const {
    return data_dir_ / db / (tbl + "." + col + ".idx");
}

bool Storage::createDatabase(const std::string& db_name) {
    auto p = dbPath(db_name);
    if (std::filesystem::exists(p)) return false;
    return std::filesystem::create_directories(p);
}

bool Storage::dropDatabase(const std::string& db_name) {
    auto p = dbPath(db_name);
    if (!std::filesystem::exists(p)) return false;
    std::filesystem::remove_all(p);
    return true;
}

bool Storage::databaseExists(const std::string& db_name) const {
    return std::filesystem::is_directory(dbPath(db_name));
}

std::vector<std::string> Storage::listDatabases() const {
    std::vector<std::string> dbs;
    if (!std::filesystem::exists(data_dir_)) return dbs;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (entry.is_directory()) {
            dbs.push_back(entry.path().filename().string());
        }
    }
    return dbs;
}

} // namespace db
