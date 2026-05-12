#include "engine/storage/storage.hpp"
#include "engine/row_codec.hpp"                 
#include "engine/cell_value.hpp"                
#include "engine/storage/storage_internal.hpp"  
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
    
    if (std::filesystem::create_directories(p)) {
        // Только БД 'system' получает системные таблицы
        if (db_name == "system") {
            initializeSystemTables(db_name);
        }
        return true;
    }
    return false;
}

void Storage::initializeSystemTables(const std::string& db_name) {
    auto make_col = [](std::string name, std::string type, bool is_pk = false) {
        ColumnDef c;
        c.name = name; c.type = type;
        c.not_null = true; c.unique = is_pk; 
        c.is_autoincrement = is_pk; 
        return c;
    };

    // Таблица пользователей (глобальная)
    TableSchema sys_users{ "sys_users", {
        make_col("user_id", "INT", true),
        make_col("username", "VARCHAR(50)"),
        make_col("password_hash", "VARCHAR(255)")
    }, 0, {} };
    sys_users.columns[1].unique = true;
    createTable(db_name, sys_users);

    // Таблица владельцев баз данных
    TableSchema sys_db_owners{ "sys_db_owners", {
        make_col("id", "INT", true),
        make_col("db_name", "VARCHAR(100)"),
        make_col("owner", "VARCHAR(50)")
    }, 0, {} };
    sys_db_owners.columns[1].unique = true;
    createTable(db_name, sys_db_owners);

    // Таблица DDL-грантов (owner может дать DDL-права другому юзеру)
    TableSchema sys_ddl_grants{ "sys_ddl_grants", {
        make_col("id", "INT", true),
        make_col("db_name", "VARCHAR(100)"),
        make_col("username", "VARCHAR(50)")
    }, 0, {} };
    createTable(db_name, sys_ddl_grants);

    auto make_row = [](const TableSchema& sch, const std::vector<std::string>& vals) {
        Row r;
        for (size_t i = 0; i < vals.size() && i < sch.columns.size(); ++i) {
            r.push_back(coerce_string_to_cell_column(sch.columns[i], vals[i], false));
        }
        return r;
    };

    // Создаём дефолтного админа
    appendRows(db_name, "sys_users", { make_row(sys_users, {"1", "admin", "admin"}) });

    // Индексы
    createIndex(db_name, "sys_users", "idx_sys_users_username", "username");
    createIndex(db_name, "sys_db_owners", "idx_sys_db_owners_dbname", "db_name");
    createIndex(db_name, "sys_ddl_grants", "idx_sys_ddl_grants_dbname", "db_name");
    createIndex(db_name, "sys_ddl_grants", "idx_sys_ddl_grants_username", "username");

    flushAllPools();
}

bool Storage::dropDatabase(const std::string& db_name) {
    auto p = dbPath(db_name);
    if (!std::filesystem::exists(p)) return false;
    
    // Закрываем все пулы, принадлежащие этой БД
    for (const auto& entry : std::filesystem::directory_iterator(p)) {
        if (entry.is_regular_file()) {
            closePool(entry.path().string());
        }
    }
    
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

std::string Storage::getDbOwner(const std::string& db_name) const {
    auto pks = indexLookup("system", "sys_db_owners", "db_name", db_name);
    if (pks.empty()) return "";
    Row row = findRow("system", "sys_db_owners", pks[0]);
    if (row.size() > 2) return cell_to_where_string(row[2]);
    return "";
}

bool Storage::hasDbDdlGrant(const std::string& db_name, const std::string& username) const {
    auto pks = indexLookup("system", "sys_ddl_grants", "db_name", db_name);
    for (const auto& pk : pks) {
        Row row = findRow("system", "sys_ddl_grants", pk);
        if (row.size() > 2 && cell_to_where_string(row[2]) == username) {
            return true;
        }
    }
    return false;
}

bool Storage::checkPrivilege(const std::string& db_name, 
                             const std::string& username,
                             const std::string& object_name, 
                             const std::string& privilege) const {
    // Новая модель: владелец имеет все права, остальные — только DML
    // Админ имеет все права всегда
    if (username == "admin") return true;
    
    std::string owner = getDbOwner(db_name);
    
    // Если пользователь — владелец БД, он имеет все права
    if (owner == username) return true;
    
    // DML-привилегии разрешены для всех пользователей
    if (privilege == "SELECT" || privilege == "INSERT" || 
        privilege == "UPDATE" || privilege == "DELETE") {
        return true;
    }
    
    // DDL — только если есть явный грант
    if (hasDbDdlGrant(db_name, username)) return true;
    
    return false;
}
} // namespace db
