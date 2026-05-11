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
        initializeSystemTables(db_name);
        return true;
    }
    return false;
}

void Storage::initializeSystemTables(const std::string& db_name) {
    auto make_col = [](std::string name, std::string type, bool is_pk = false, std::string ref_tbl = "", std::string ref_col = "") {
        ColumnDef c;
        c.name = name; c.type = type;
        c.not_null = true; c.unique = is_pk; 
        c.is_autoincrement = is_pk; 
        c.fk_ref_table = ref_tbl; c.fk_ref_column = ref_col;
        if (!ref_tbl.empty()) { c.on_delete = OnDeleteAction::CASCADE; c.on_update = OnUpdateAction::CASCADE; }
        return c;
    };

    TableSchema sys_users{ "sys_users", {
        make_col("user_id", "INT", true),
        make_col("username", "VARCHAR(50)"),
        make_col("password_hash", "VARCHAR(255)")
    }, 0, {} };
    sys_users.columns[1].unique = true;
    createTable(db_name, sys_users);

    TableSchema sys_roles{ "sys_roles", {
        make_col("role_id", "INT", true),
        make_col("role_name", "VARCHAR(50)")
    }, 0, {} };
    sys_roles.columns[1].unique = true;
    createTable(db_name, sys_roles);

    TableSchema sys_ur{ "sys_user_roles", {
        make_col("id", "INT", true),
        make_col("user_id", "INT", false, "sys_users", "user_id"),
        make_col("role_id", "INT", false, "sys_roles", "role_id")
    }, 0, {} };
    createTable(db_name, sys_ur);

    TableSchema sys_grants{ "sys_grants", {
        make_col("grant_id", "INT", true),
        make_col("role_id", "INT", false, "sys_roles", "role_id"),
        make_col("object_name", "VARCHAR(100)"),
        make_col("privilege", "VARCHAR(50)")
    }, 0, {} };
    createTable(db_name, sys_grants);

    auto make_row = [](const TableSchema& sch, const std::vector<std::string>& vals) {
        Row r;
        for (size_t i = 0; i < vals.size() && i < sch.columns.size(); ++i) {
            r.push_back(coerce_string_to_cell_column(sch.columns[i], vals[i], false));
        }
        return r;
    };

    // Adding basic data
    appendRows(db_name, "sys_users", { make_row(sys_users, {"1", "admin", "admin"}) });
    appendRows(db_name, "sys_roles", { make_row(sys_roles, {"1", "superuser"}) });
    appendRows(db_name, "sys_user_roles", { make_row(sys_ur, {"1", "1", "1"}) });
    appendRows(db_name, "sys_grants", { make_row(sys_grants, {"1", "1", "*", "ALL"}) });

    // 3. Creating indexes
    createIndex(db_name, "sys_users", "idx_sys_users_username", "username");
    createIndex(db_name, "sys_roles", "idx_sys_roles_name", "role_name");
    createIndex(db_name, "sys_user_roles", "idx_sys_ur_userid", "user_id");
    createIndex(db_name, "sys_grants", "idx_sys_grants_roleid", "role_id");

    flushAllPools();
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

bool Storage::checkPrivilege(const std::string& db_name, 
                             const std::string& username,
                             const std::string& object_name, 
                             const std::string& privilege) const {
    
    // 1. Ищем user_id по имени
    auto users_pks = indexLookup(db_name, "sys_users", "username", username);
    if (users_pks.empty()) return false;
    std::string user_id = users_pks[0];
    
    if (user_id.empty()) return false;

    // 2. Ищем все role_id для этого пользователя
    auto ur_pks = indexLookup(db_name, "sys_user_roles", "user_id", user_id);
    if (ur_pks.empty()) return false;

    std::vector<std::string> role_ids;
    for(const auto& pk : ur_pks) {
        Row ur_row = findRow(db_name, "sys_user_roles", pk);
        if(!ur_row.empty() && ur_row.size() > 2) { 
            std::string r_id = cell_to_where_string(ur_row[2]);
            if (!r_id.empty())
                role_ids.push_back(r_id);
        }
    }

    if (role_ids.empty()) return false;

    // 3. Проверяем таблицу sys_grants
    for (const auto& r_id : role_ids) {
        auto grant_pks = indexLookup(db_name, "sys_grants", "role_id", r_id);
        for(const auto& pk : grant_pks) {
             Row g_row = findRow(db_name, "sys_grants", pk);
             if(!g_row.empty() && g_row.size() > 3) {
                 std::string g_obj  = cell_to_where_string(g_row[2]);
                 std::string g_priv = cell_to_where_string(g_row[3]);
                 
                 bool object_matches = (g_obj == object_name || g_obj == "*");
                 bool priv_matches = (g_priv == privilege || g_priv == "ALL");

                 if (object_matches && priv_matches) return true;
             }
        }
    }

    return false;
}
} // namespace db
