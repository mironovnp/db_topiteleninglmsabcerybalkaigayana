#include "engine/executor.hpp"
#include "engine/wal.hpp"
#include "engine/page.hpp"
#include "engine/cell_value.hpp"
#include "engine/row_codec.hpp"
#include "engine/storage/storage.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class TestSuite {
public:
    TestSuite(const std::string& data_dir) : executor(data_dir), data_dir(data_dir) {
        setup();
    }

    void setup() {
        std::cout << "=== Подготовка тестового окружения ===" << std::endl;
        try {
            if (std::filesystem::exists(data_dir)) {
                std::filesystem::remove_all(data_dir);
            }
            std::filesystem::create_directory(data_dir);
        } catch (const std::exception& e) {
            std::cerr << "Ошибка при подготовке папки данных: " << e.what() << std::endl;
        }
    }

    void run_all() {
        std::cout << "\n>>> ФАЗА 1: DDL и Базовые ошибки" << std::endl;
        test_ddl_and_errors();

        std::cout << "\n>>> ФАЗА 2: Базовый DML (Insert, Update, Delete)" << std::endl;
        test_basic_dml();

        std::cout << "\n>>> ФАЗА 3: SELECT, Выражения и Сортировка" << std::endl;
        test_select_features();

        std::cout << "\n>>> ФАЗА 4: JOIN-ы (INNER, LEFT, RIGHT, FULL, CROSS)" << std::endl;
        test_joins();

        std::cout << "\n>>> ФАЗА 5: Группировка и Агрегаты" << std::endl;
        test_group_by_aggregates();

        std::cout << "\n>>> ФАЗА 6: Подзапросы (IN, EXISTS, Correlated)" << std::endl;
        test_subqueries();

        std::cout << "\n>>> ФАЗА 7: Псевдонимы (Aliases)" << std::endl;
        test_aliases();

        std::cout << "\n>>> ФАЗА 8: Индексы и ALTER TABLE" << std::endl;
        test_indexes_and_alter();

        std::cout << "\n>>> ФАЗА 9: Ограничения (NOT NULL, UNIQUE, PK)" << std::endl;
        test_constraints();

        std::cout << "\n>>> ФАЗА 10: Продвинутые функции (DISTINCT, IS NULL, FK)" << std::endl;
        test_advanced_features();

        std::cout << "\n>>> ФАЗА 11: Каскады и сложная агрегация" << std::endl;
        test_cascades_and_aggr_distinct();

        std::cout << "\n>>> ФАЗА 12: Поиск и фильтрация (LIKE, BETWEEN)" << std::endl;
        test_search_and_filter();

        std::cout << "\n>>> ФАЗА 13: Продвинутые подзапросы" << std::endl;
        test_advanced_subqueries();

        std::cout << "\n>>> ФАЗА 14: Автоматизация и оптимизация индексов" << std::endl;
        test_auto_default_index_ranges();

        std::cout << "\n>>> ФАЗА 15: Обработка ошибок и краевых случаев" << std::endl;
        test_errors_and_edge_cases();

        std::cout << "\n>>> ФАЗА 16: Команды SHOW" << std::endl;
        test_show_commands();

        std::cout << "\n>>> ФАЗА 17: Удаление БД" << std::endl;
        test_drop_db();

        std::cout << "\n>>> ФАЗА 18: WAL Recovery" << std::endl;
        test_wal_recovery();

        std::cout << "\n>>> ФАЗА 19: Logical WAL Recovery (Row-level)" << std::endl;
        test_logical_wal_recovery();

        std::cout << "\n>>> ФАЗА 20: Типизированное хранение и NULL" << std::endl;
        test_cell_value_and_nulls();

        std::cout << "\n>>> ФАЗА 21: Типизированные ключи B+-дерева (wire, legacy, compare)" << std::endl;
        test_btree_typed_keys();

        std::cout << "\n>>> ФАЗА 22: LOAD CSV" << std::endl;
        test_load_csv();
        test_load_csv_nonempty_and_append();

        std::cout << "\n>>> ФАЗА 23: Многострочные запросы и Алиасы агрегатов" << std::endl;
        test_multi_line_and_aggr_aliases();

        std::cout << "\n>>> ФАЗА 24: Проверка RBAC" << std::endl;
        test_rbac();

        std::cout << "\n" << std::string(40, '=') << std::endl;
        std::cout << "ИТОГО: " << passed_count << "/" << total_count << " тестов пройдено." << std::endl;
        if (passed_count < total_count) {
            std::cout << "ЕСТЬ ОШИБКИ! Проверьте вывод выше." << std::endl;
        } else {
            std::cout << "ВСЕ ТЕСТЫ УСПЕШНО ЗАВЕРШЕНЫ!" << std::endl;
        }
        std::cout << std::string(40, '=') << std::endl;
    }

private:
    db::Executor executor;
    std::string data_dir;
    int total_count = 0;
    int passed_count = 0;

    void assert_success(const std::string& name, const std::string& sql) {
        total_count++;
        json res = executor.execute(sql);
        if (res["success"].get<bool>()) {
            std::cout << "  [OK] " << name << std::endl;
            passed_count++;
        } else {
            std::cerr << "  [FAIL] " << name << "\n       SQL: " << sql << "\n       Error: " << res["message"] << std::endl;
        }
    }

    void assert_error(const std::string& name, const std::string& sql, const std::string& expected_part = "") {
        total_count++;
        json res = executor.execute(sql);
        if (!res["success"].get<bool>()) {
            std::string msg = res["message"];
            if (expected_part.empty() || msg.find(expected_part) != std::string::npos) {
                std::cout << "  [OK] " << name << " (Expected error caught)" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] " << name << "\n       SQL: " << sql << "\n       Got error: " << msg << "\n       Expected part: " << expected_part << std::endl;
            }
        } else {
            std::cerr << "  [FAIL] " << name << " (Expected error, but query succeeded)\n       SQL: " << sql << std::endl;
        }
    }

    void assert_rows(const std::string& name, const std::string& sql, int expected_count, const std::vector<std::vector<std::string>>& values = {}) {
        total_count++;
        json res = executor.execute(sql);
        if (!res["success"].get<bool>()) {
            std::cerr << "  [FAIL] " << name << " (Execute error: " << res["message"] << ")\n       SQL: " << sql << std::endl;
            return;
        }

        auto& rows = res["rows"];
        if (rows.size() != (size_t)expected_count) {
            std::cerr << "  [FAIL] " << name << " (Row count mismatch: got " << rows.size() << ", expected " << expected_count << ")\n       SQL: " << sql << std::endl;
            return;
        }

        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                for (size_t j = 0; j < values[i].size(); ++j) {
                    std::string actual = rows[i][j].is_null() ? "NULL" : rows[i][j].get<std::string>();
                    bool match = (actual == values[i][j]);
                    if (!match) {
                        try {
                            if (actual.find('.') != std::string::npos || values[i][j].find('.') != std::string::npos) {
                                if (std::stod(actual) == std::stod(values[i][j])) match = true;
                            }
                        } catch(...) {}
                    }

                    if (!match) {
                         std::cerr << "  [FAIL] " << name << " (Value mismatch at row " << i << " col " << j << ": got '" << actual << "', expected '" << values[i][j] << "')\n       SQL: " << sql << std::endl;
                         return;
                    }
                }
            }
        }

        std::cout << "  [OK] " << name << std::endl;
        passed_count++;
    }

    void assert_affected(const std::string& name, const std::string& sql, int expected_count) {
        total_count++;
        json res = executor.execute(sql);
        if (!res["success"].get<bool>()) {
            std::cerr << "  [FAIL] " << name << " (Execute error: " << res["message"] << ")\n       SQL: " << sql << std::endl;
            return;
        }
        int actual = res.contains("rows_affected") ? res["rows_affected"].get<int>() : 0;
        if (actual != expected_count) {
            std::cerr << "  [FAIL] " << name << " (Affected mismatch: got " << actual << ", expected " << expected_count << ")\n       SQL: " << sql << std::endl;
            return;
        }
        std::cout << "  [OK] " << name << std::endl;
        passed_count++;
    }

    void test_ddl_and_errors() {
        assert_error("Запрос без базы данных", "CREATE TABLE t (id INT);", "No database selected");
        assert_error("SHOW TABLES без базы", "SHOW TABLES;", "No database selected");
        assert_success("Создание БД test_db", "CREATE DATABASE test_db;");
        assert_success("Использование БД", "USE test_db;");
        assert_success("Создание таблицы users", "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);");
        assert_success("Создание таблицы orders", "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount FLOAT);");
        
        executor.execute("INSERT INTO users (id, name, age) VALUES (0, 'System', 0);");
        assert_error("Несуществующая колонка", "SELECT non_existent FROM users;", "Unknown column");
        executor.execute("DELETE FROM users;");
    }

    void test_basic_dml() {
        assert_success("Вставка в users (1)", "INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25);");
        assert_success("Вставка в users (2)", "INSERT INTO users (id, name, age) VALUES (2, 'Bob', 30);");
        assert_success("Вставка в users (3)", "INSERT INTO users (id, name, age) VALUES (3, 'Charlie', 20);");
        
        assert_rows("Проверка вставки", "SELECT name FROM users ORDER BY id;", 3, {{"Alice"}, {"Bob"}, {"Charlie"}});
        
        assert_success("Update возраста Bob", "UPDATE users SET age = 31 WHERE name = 'Bob';");
        assert_rows("Проверка Update", "SELECT age FROM users WHERE name = 'Bob';", 1, {{"31"}});
        
        assert_success("Delete Charlie", "DELETE FROM users WHERE age < 25;");
        assert_rows("Проверка Delete", "SELECT count(*) FROM users;", 1, {{"2"}});
    }

    void test_select_features() {
        executor.execute("INSERT INTO users (id, name, age) VALUES (3, 'David', 40);");
        
        assert_rows("Выражения в SELECT", "SELECT age + 10, age * 2 FROM users WHERE id = 1;", 1, {{"35.000", "50.000"}});
        assert_rows("Псевдонимы (AS)", "SELECT name AS username FROM users WHERE id = 2;", 1, {{"Bob"}});
        assert_rows("Сложный WHERE (AND/OR)", "SELECT name FROM users WHERE age > 25 AND age < 35 OR name = 'David';", 2, {{"Bob"}, {"David"}});
        assert_rows("ORDER BY DESC", "SELECT name FROM users ORDER BY age DESC;", 3, {{"David"}, {"Bob"}, {"Alice"}});
        assert_rows("LIMIT и OFFSET", "SELECT name FROM users ORDER BY id LIMIT 1 OFFSET 1;", 1, {{"Bob"}});
    }

    void test_joins() {
        executor.execute("DELETE FROM orders;");
        executor.execute("INSERT INTO orders (id, user_id, amount) VALUES (101, 1, 50.5);");
        executor.execute("INSERT INTO orders (id, user_id, amount) VALUES (102, 1, 150.0);");
        executor.execute("INSERT INTO orders (id, user_id, amount) VALUES (103, 4, 20.0);");
        
        assert_rows("INNER JOIN", 
            "SELECT users.name, orders.amount FROM users INNER JOIN orders ON users.id = orders.user_id ORDER BY orders.id;", 
            2, {{"Alice", "50.5"}, {"Alice", "150.0"}});

        assert_rows("LEFT JOIN", 
            "SELECT users.name, orders.amount FROM users LEFT JOIN orders ON users.id = orders.user_id ORDER BY users.id, orders.amount;", 
            4, {{"Alice", "50.5"}, {"Alice", "150.0"}, {"Bob", ""}, {"David", ""}});

        assert_rows("RIGHT JOIN", 
            "SELECT users.name, orders.amount FROM users RIGHT JOIN orders ON users.id = orders.user_id ORDER BY orders.id;", 
            3, {{"Alice", "50.5"}, {"Alice", "150.0"}, {"", "20.0"}});

        assert_rows("CROSS JOIN", 
            "SELECT users.name, orders.id FROM users CROSS JOIN orders WHERE users.id = 1 AND orders.id = 101;", 
            1, {{"Alice", "101"}});
            
        assert_rows("FULL JOIN",
            "SELECT users.name, orders.id FROM users FULL JOIN orders ON users.id = orders.user_id ORDER BY users.id DESC, orders.id ASC;",
            5, {{"David", ""}, {"Bob", ""}, {"Alice", "101"}, {"Alice", "102"}, {"", "103"}});
    }

    void test_group_by_aggregates() {
        assert_rows("Агрегаты без группировки", "SELECT COUNT(*), SUM(age) FROM users;", 1, {{"3", "96.000"}});
        
        executor.execute("INSERT INTO orders (id, user_id, amount) VALUES (104, 2, 10.0);");
        assert_rows("GROUP BY с агрегатами", 
            "SELECT user_id, SUM(amount) FROM orders GROUP BY user_id ORDER BY user_id;", 
            3, {{"1", "200.500"}, {"2", "10.000"}, {"4", "20.000"}});

        assert_rows("HAVING фильтрация", 
            "SELECT user_id, SUM(amount) FROM orders GROUP BY user_id HAVING SUM(amount) > 50;", 
            1, {{"1", "200.500"}});
    }

    void test_subqueries() {
        assert_rows("Подзапрос IN", 
            "SELECT name FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > 100);", 
            1, {{"Alice"}});

        assert_rows("Подзапрос EXISTS", 
            "SELECT name FROM users WHERE EXISTS (SELECT * FROM orders WHERE orders.user_id = users.id);", 
            2, {{"Alice"}, {"Bob"}});

        assert_rows("Фильтрация по значению", 
            "SELECT name FROM users WHERE age > 30 ORDER BY id;", 
            2, {{"Bob"}, {"David"}});
    }

    void test_aliases() {
        // Алиасы таблиц в JOIN
        assert_rows("Алиасы таблиц в JOIN", 
            "SELECT u.name, o.amount FROM users AS u INNER JOIN orders AS o ON u.id = o.user_id WHERE u.id = 1;", 
            2, {{"Alice", "50.5"}, {"Alice", "150.0"}});

        // Алиасы столбцов
        assert_rows("Алиасы столбцов (AS)", 
            "SELECT name AS login, id AS uid FROM users WHERE id = 1;", 
            1, {{"Alice", "1"}});

        // Алиасы таблиц без AS
        assert_rows("Алиасы без AS", 
            "SELECT usr.name FROM users usr WHERE usr.id = 2;", 
            1, {{"Bob"}});
            
        // Смешанные алиасы в сложном запросе (алиасы в WHERE по стандарту не используются)
        assert_rows("Смешанные алиасы", 
            "SELECT u.name AS n, o.amount AS a FROM users u JOIN orders o ON u.id = o.user_id WHERE o.amount > 100;", 
            1, {{"Alice", "150.0"}});
    }

    void test_indexes_and_alter() {
        assert_success("Создание индекса на users.name", "CREATE INDEX idx_name ON users(name);");
        assert_rows("Поиск по индексу", "SELECT id FROM users WHERE name = 'Alice';", 1, {{"1"}});

        assert_success("ALTER TABLE ADD COLUMN", "ALTER TABLE users ADD COLUMN email TEXT;");
        executor.execute("UPDATE users SET email = 'alice@example.com' WHERE id = 1;");
        assert_rows("Проверка новой колонки", "SELECT email FROM users WHERE id = 1;", 1, {{"alice@example.com"}});

        assert_success("ALTER TABLE DROP COLUMN", "ALTER TABLE users DROP COLUMN email;");
        executor.execute("INSERT INTO users (id, name) VALUES (10, 'Temp');");
        assert_error("Проверка удаления колонки", "SELECT email FROM users;", "Unknown column");
    }
    void test_constraints() {
        assert_success("Создание таблицы с ограничениями", 
            "CREATE TABLE employees (id INT PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE, salary FLOAT);");

        assert_success("Корректная вставка", 
            "INSERT INTO employees (id, name, email, salary) VALUES (1, 'John', 'john@test.com', 5000);");

        // Тест NOT NULL
        assert_error("Ошибка NOT NULL (name)", 
            "INSERT INTO employees (id, name, email) VALUES (2, NULL, 'test@test.com');", "NOT NULL");

        // Тест PRIMARY KEY
        assert_error("Ошибка PRIMARY KEY (дубликат id)", 
            "INSERT INTO employees (id, name, email) VALUES (1, 'Alice', 'alice@test.com');", "PRIMARY KEY");

        // Тест UNIQUE
        assert_error("Ошибка UNIQUE (дубликат email)", 
            "INSERT INTO employees (id, name, email) VALUES (2, 'Bob', 'john@test.com');", "UNIQUE");

        // Тест UNIQUE в рамках одного запроса
        assert_error("Ошибка UNIQUE (дубликат в списке VALUES)", 
            "INSERT INTO employees (id, name, email) VALUES (3, 'X', 'x@t.com'), (4, 'Y', 'x@t.com');", "insert list");

        assert_rows("Проверка, что ошибочные данные не вставились", 
            "SELECT count(*) FROM employees;", 1, {{"1"}});
    }
    void test_advanced_features() {
        // DISTINCT
        executor.execute("INSERT INTO users (id, name, age) VALUES (100, 'User1', 20), (101, 'User2', 20);");
        assert_rows("DISTINCT по возрасту", "SELECT DISTINCT age FROM users WHERE id >= 100;", 1, {{"20"}});

        // IS NULL / IS NOT NULL
        executor.execute("INSERT INTO employees (id, name, email) VALUES (50, 'NoEmail', NULL);");
        assert_rows("IS NULL поиск", "SELECT name FROM employees WHERE email IS NULL;", 1, {{"NoEmail"}});
        assert_rows("IS NOT NULL поиск", "SELECT name FROM employees WHERE id = 50 AND email IS NOT NULL;", 0);

        // FOREIGN KEY
        assert_success("Создание таблицы с FK", "CREATE TABLE posts (id INT PRIMARY KEY, author_id INT REFERENCES users(id), title TEXT);");
        assert_success("FK вставка (существующий родитель)", "INSERT INTO posts (id, author_id, title) VALUES (1, 1, 'Hello');");
        assert_error("FK ошибка (несуществующий родитель)", "INSERT INTO posts (id, author_id, title) VALUES (2, 999, 'Bad');", "FOREIGN KEY violation");
    }

    void test_drop_db() {
        assert_success("Удаление БД", "DROP DATABASE test_db;");
        assert_error("Проверка удаления БД", "USE test_db;", "does not exist");
    }

    void test_wal_recovery() {
        total_count++;
        try {
            auto dir = std::filesystem::path(data_dir) / "wal_recovery";
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);

            std::string data_file = (dir / "t.db").string();
            std::string wal_file = (dir / "wal.log").string();

            // Create an "old" page on disk.
            db::Page oldp;
            oldp.reset();
            oldp.setPageType(db::LEAF_PAGE);
            oldp.setPageId(0);
            oldp.setLSN(0);
            const char old_marker[] = "OLD";
            memcpy(oldp.data + 128, old_marker, sizeof(old_marker));
            {
                std::ofstream out(data_file, std::ios::binary | std::ios::trunc);
                out.write(oldp.data, db::PAGE_SIZE);
            }

            // Create a PAGE_IMAGE record with a different marker.
            db::Page newp;
            newp.reset();
            newp.setPageType(db::LEAF_PAGE);
            newp.setPageId(0);
            newp.setLSN(0); // will be overwritten during recovery with record.lsn
            const char new_marker[] = "NEW";
            memcpy(newp.data + 128, new_marker, sizeof(new_marker));

            db::WALManager wal(wal_file);
            uint16_t fpl = static_cast<uint16_t>(data_file.size());
            std::string payload;
            payload.reserve(2 + data_file.size() + db::PAGE_SIZE);
            payload.append(reinterpret_cast<const char*>(&fpl), 2);
            payload.append(data_file.data(), data_file.size());
            payload.append(newp.data, db::PAGE_SIZE);

            db::LogRecord rec(0, 0, db::LogRecordType::PAGE_IMAGE, 0, std::move(payload));
            db::LSN lsn = wal.appendRecord(rec);
            wal.flushTo(lsn);

            // Apply recovery and validate disk is updated.
            wal.recover();

            db::Page got;
            {
                std::ifstream in(data_file, std::ios::binary);
                in.read(got.data, db::PAGE_SIZE);
            }

            bool ok_marker = (memcmp(got.data + 128, new_marker, sizeof(new_marker)) == 0);
            bool ok_lsn = (got.getLSN() == lsn);

            if (ok_marker && ok_lsn) {
                std::cout << "  [OK] WAL recovery применяет PAGE_IMAGE" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] WAL recovery не применился: marker=" << ok_marker
                          << " lsn=" << ok_lsn << " (got=" << got.getLSN()
                          << " expected=" << lsn << ")" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] WAL recovery exception: " << e.what() << std::endl;
        }
    }
    void test_cascades_and_aggr_distinct() {
        // COUNT DISTINCT
        executor.execute("INSERT INTO users (id, name, age) VALUES (200, 'A', 30), (201, 'B', 30), (202, 'C', 40);");
        assert_rows("COUNT(DISTINCT age)", "SELECT COUNT(DISTINCT age) FROM users WHERE id >= 200;", 1, {{"2"}});

        // CASCADE DELETE
        executor.execute("CREATE TABLE comments (id INT PRIMARY KEY, user_id INT REFERENCES users(id) ON DELETE CASCADE, body TEXT);");
        executor.execute("INSERT INTO comments (id, user_id, body) VALUES (1, 200, 'Comm1'), (2, 200, 'Comm2'), (3, 201, 'Comm3');");
        
        executor.execute("DELETE FROM users WHERE id = 200;");
        assert_rows("Проверка каскадного удаления", "SELECT count(*) FROM comments WHERE user_id = 200;", 1, {{"0"}});
        assert_rows("Другие комментарии остались", "SELECT count(*) FROM comments WHERE user_id = 201;", 1, {{"1"}});

        // RESTRICT
        executor.execute("CREATE TABLE logs (id INT PRIMARY KEY, user_id INT REFERENCES users(id));"); // NO_ACTION by default
        executor.execute("INSERT INTO logs (id, user_id) VALUES (1, 201);");
        assert_error("RESTRICT блокировка удаления", "DELETE FROM users WHERE id = 201;", "FOREIGN KEY violation");
        
        executor.execute("DROP TABLE logs;");
        executor.execute("DROP TABLE comments;"); // Also clean up comments

        // CASCADE UPDATE
        executor.execute("CREATE TABLE profiles (id INT PRIMARY KEY, u_id INT REFERENCES users(id) ON UPDATE CASCADE);");
        executor.execute("INSERT INTO profiles (id, u_id) VALUES (1, 201);");
        executor.execute("UPDATE users SET id = 222 WHERE id = 201;");
        assert_rows("Проверка каскадного обновления", "SELECT u_id FROM profiles WHERE id = 1;", 1, {{"222"}});
    }
    void test_search_and_filter() {
        // LIKE
        executor.execute("INSERT INTO users (id, name, age) VALUES (300, 'Alexander', 25), (301, 'Alex', 22), (302, 'Boris', 30);");
        assert_rows("LIKE поиск (% в конце)", "SELECT count(*) FROM users WHERE name LIKE 'Alex%';", 1, {{"2"}});
        assert_rows("LIKE поиск (% в начале)", "SELECT count(*) FROM users WHERE name LIKE '%der';", 1, {{"1"}});
        assert_rows("LIKE поиск (_ символ)", "SELECT count(*) FROM users WHERE name LIKE 'Al_x';", 1, {{"1"}});
        assert_rows("NOT LIKE", "SELECT count(*) FROM users WHERE id >= 300 AND name NOT LIKE 'Alex%';", 1, {{"1"}});

        // BETWEEN
        assert_rows("BETWEEN для чисел", "SELECT count(*) FROM users WHERE id >= 300 AND age BETWEEN 25 AND 35;", 1, {{"2"}});
        assert_rows("NOT BETWEEN", "SELECT count(*) FROM users WHERE id >= 300 AND age NOT BETWEEN 20 AND 25;", 1, {{"1"}});
    }
    void test_advanced_subqueries() {
        executor.execute("CREATE DATABASE sub_db;");
        executor.execute("USE sub_db;");
        executor.execute("CREATE TABLE users (id INT PRIMARY KEY, name TEXT, age INT);");
        executor.execute("INSERT INTO users (id, name, age) VALUES (1, 'Alice', 30), (2, 'Alexander', 22), (3, 'Boris', 35);");

        // Scalar subquery in WHERE
        assert_rows("Скалярный подзапрос в WHERE", 
            "SELECT name FROM users WHERE age = (SELECT age FROM users WHERE name = 'Boris');", 1, {{"Boris"}});

        // Scalar subquery in SELECT
        assert_rows("Подзапрос в списке SELECT",
            "SELECT name, (SELECT count(*) FROM users WHERE age < 30) as young_count FROM users WHERE name = 'Alexander';", 
            1, {{"Alexander", "1"}});

        // Subquery in FROM (Derived Table)
        assert_rows("Подзапрос в FROM (Derived Table)",
            "SELECT count(*) FROM (SELECT name FROM users WHERE age > 25) AS old_users;",
            1, {{"2"}}); // Alice (30) and Boris (35)
        
        executor.execute("USE test_db;"); // Switch back
        executor.execute("DROP DATABASE sub_db;");
    }
    void test_auto_default_index_ranges() {
        executor.execute("CREATE DATABASE opt_db;");
        executor.execute("USE opt_db;");
        
        // AUTOINCREMENT & DEFAULT
        executor.execute("CREATE TABLE items (id INT PRIMARY KEY AUTOINCREMENT, name TEXT, category TEXT DEFAULT 'General');");
        executor.execute("INSERT INTO items (name) VALUES ('Item A');"); // id should be 1, category General
        executor.execute("INSERT INTO items (name, category) VALUES ('Item B', 'Tools');"); // id 2
        executor.execute("INSERT INTO items (name) VALUES ('Item C');"); // id 3
        
        assert_rows("Проверка AUTOINCREMENT", "SELECT id FROM items WHERE name = 'Item C';", 1, {{"3"}});
        assert_rows("Проверка DEFAULT", "SELECT category FROM items WHERE name = 'Item A';", 1, {{"General"}});

        // Index Ranges
        executor.execute("CREATE INDEX idx_id ON items(id);");
        assert_rows("Index Range (>)", "SELECT count(*) FROM items WHERE id > 1;", 1, {{"2"}});
        assert_rows("Index Range (BETWEEN)", "SELECT count(*) FROM items WHERE id BETWEEN 1 AND 2;", 1, {{"2"}});
        assert_rows("Index Range (<=)", "SELECT count(*) FROM items WHERE id <= 2;", 1, {{"2"}});

        // Index Sort Optimization
        assert_rows("Index Sort (ASC)", "SELECT id FROM items ORDER BY id;", 3, {{"1"}, {"2"}, {"3"}});
        assert_rows("Index Sort (DESC)", "SELECT id FROM items ORDER BY id DESC;", 3, {{"3"}, {"2"}, {"1"}});

        executor.execute("USE test_db;");
        executor.execute("DROP DATABASE opt_db;");
    }

    void test_errors_and_edge_cases() {
        // 1. Несуществующие объекты
        assert_error("SELECT из несуществующей таблицы", "SELECT * FROM ghost_table;", "does not exist");
        assert_error("INSERT в несуществующую таблицу", "INSERT INTO ghost_table (id) VALUES (1);", "does not exist");
        assert_error("UPDATE несуществующей таблицы", "UPDATE ghost_table SET id = 1;", "does not exist");
        assert_error("DELETE из несуществующей таблицы", "DELETE FROM ghost_table;", "does not exist");
        assert_error("DROP TABLE без IF EXISTS", "DROP TABLE ghost_table;", "does not exist");
        assert_success("DROP TABLE с IF EXISTS", "DROP TABLE IF EXISTS ghost_table;");
        assert_error("DROP DATABASE без IF EXISTS", "DROP DATABASE ghost_db;", "does not exist");
        assert_success("DROP DATABASE с IF EXISTS", "DROP DATABASE IF EXISTS ghost_db;");

        // 2. Дубликаты
        assert_error("Создание дубликата таблицы", "CREATE TABLE users (id INT);", "already exists");
        executor.execute("CREATE DATABASE dup_db;");
        assert_error("Создание дубликата БД", "CREATE DATABASE dup_db;", "already exists");
        executor.execute("DROP DATABASE dup_db;");

        // 3. Синтаксические и логические ошибки в DML
        assert_error("INSERT: неизвестная колонка", "INSERT INTO users (unknown_col) VALUES (1);", "Unknown column");
        assert_error("UPDATE: неизвестная колонка в SET", "UPDATE users SET unknown_col = 1;", "Unknown column");
        assert_error("UPDATE: неизвестная колонка в WHERE", "UPDATE users SET age = 30 WHERE unknown_col = 1;", "Unknown column");
        assert_error("DELETE: неизвестная колонка в WHERE", "DELETE FROM users WHERE unknown_col = 1;", "Unknown column");

        // 4. Краевые случаи DML
        executor.execute("CREATE TABLE empty_table (id INT PRIMARY KEY);");
        assert_affected("DELETE из пустой таблицы", "DELETE FROM empty_table;", 0);
        assert_affected("UPDATE в пустой таблице", "UPDATE empty_table SET id = 1;", 0);
        executor.execute("DROP TABLE empty_table;");

        executor.execute("CREATE TABLE mass_table (id INT PRIMARY KEY, val TEXT);");
        executor.execute("INSERT INTO mass_table (id, val) VALUES (1, 'A'), (2, 'A'), (3, 'B');");
        assert_affected("Массовый UPDATE (2 строки)", "UPDATE mass_table SET val = 'C' WHERE val = 'A';", 2);
        assert_affected("Массовый DELETE (все)", "DELETE FROM mass_table;", 3);
        executor.execute("DROP TABLE mass_table;");

        // 5. Работа с типами
        executor.execute("CREATE TABLE types_table (id INT PRIMARY KEY, f FLOAT, b BOOL);");
        executor.execute("INSERT INTO types_table (id, f, b) VALUES (1, 10.5, TRUE);");
        assert_rows("Сравнение FLOAT", "SELECT id FROM types_table WHERE f > 10.0;", 1, {{"1"}});
        assert_rows("Поиск по BOOL", "SELECT id FROM types_table WHERE b = TRUE;", 1, {{"1"}});
        executor.execute("DROP TABLE types_table;");
    }

    void test_show_commands() {
        // 1. SHOW DATABASES
        // Should contain test_db
        total_count++;
        json res = executor.execute("SHOW DATABASES;");
        bool found_db = false;
        for (const auto& row : res["rows"]) {
            if (row[0].get<std::string>() == "test_db") { found_db = true; break; }
        }
        if (found_db) {
            std::cout << "  [OK] SHOW DATABASES содержит test_db" << std::endl;
            passed_count++;
        } else {
            std::cerr << "  [FAIL] SHOW DATABASES не нашел test_db" << std::endl;
        }

        // 2. SHOW TABLES
        // We don't know exact count because previous tests might have left some tables.
        // Let's just check that users and orders exist.
        total_count++;
        res = executor.execute("SHOW TABLES;");
        bool found_users = false, found_orders = false;
        for (const auto& row : res["rows"]) {
            if (row[0].get<std::string>() == "users") found_users = true;
            if (row[0].get<std::string>() == "orders") found_orders = true;
        }
        if (found_users && found_orders) {
            std::cout << "  [OK] SHOW TABLES содержит users и orders" << std::endl;
            passed_count++;
        } else {
            std::cerr << "  [FAIL] SHOW TABLES не нашел нужные таблицы" << std::endl;
        }
        
        // 3. SHOW COLUMNS
        assert_rows("SHOW COLUMNS FROM users", "SHOW COLUMNS FROM users;", 3, {
            {"id", "INT", "YES", "PRI", "NULL", ""},
            {"name", "TEXT", "YES", "", "NULL", ""},
            {"age", "INT", "YES", "", "NULL", ""}
        });

        // 4. SHOW INDEX
        assert_rows("SHOW INDEX FROM users", "SHOW INDEX FROM users;", 2, {
            {"users", "0", "PRIMARY", "1", "id"},
            {"users", "1", "idx_name", "1", "name"}
        });

        // 5. SHOW CREATE TABLE
        total_count++;
        res = executor.execute("SHOW CREATE TABLE users;");
        if (res["success"].get<bool>() && res["rows"].size() == 1) {
            std::string sql = res["rows"][0][1];
            if (sql.find("CREATE TABLE users") != std::string::npos && sql.find("id INT PRIMARY KEY") != std::string::npos) {
                std::cout << "  [OK] SHOW CREATE TABLE users корректен" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] SHOW CREATE TABLE users вернул странный SQL: " << sql << std::endl;
            }
        } else {
            std::cerr << "  [FAIL] SHOW CREATE TABLE users ошибка: " << res["message"] << std::endl;
        }

        // 6. Ошибки SHOW (с базой, но на несуществующих таблицах)
        assert_error("SHOW COLUMNS для призрака", "SHOW COLUMNS FROM ghost_table;", "does not exist");
        assert_error("SHOW INDEX для призрака", "SHOW INDEX FROM ghost_table;", "does not exist");
        assert_error("SHOW CREATE TABLE для призрака", "SHOW CREATE TABLE ghost_table;", "does not exist");
    }

    void test_logical_wal_recovery() {
        total_count++;
        try {
            auto dir = std::filesystem::path(data_dir) / "logical_wal";
            if (std::filesystem::exists(dir)) std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);

            std::string db_name = "recovery_db";
            std::string table_path;
            {
                db::Storage storage(dir.string());
                storage.createDatabase(db_name);
                
                db::TableSchema s;
                s.table_name = "items";
                s.columns = {{"id", "INT"}, {"val", "TEXT"}};
                s.primary_key_index = 0;
                storage.createTable(db_name, s);
                storage.createIndex(db_name, "items", "idx_val", "val");
                table_path = (dir / db_name / "items.db").string();

                // Вставим строку физически для теста удаления
                db::Row row2;
                row2.push_back(db::CellPrimitive{int64_t{20}});
                row2.push_back(db::CellPrimitive{std::string{"ToDelete"}});
                storage.appendRows(db_name, "items", {row2});
                storage.indexInsertRow(db_name, "items", storage.getTableSchema(db_name, "items"), row2);
            } // Сброс всех BufferPool на диск
            
            db::Storage storage(dir.string());
            std::string wal_path = (dir / db_name / "wal.log").string();
            db::WALManager wal(wal_path);

            // 1. Тест ROW_UPSERT (Восстановление вставки)
            table_path = (dir / db_name / "items.db").string();
            std::string index_path = (dir / db_name / "items.val.idx").string();
            std::string key1 = "10";
            db::TableSchema tbl_sch;
            tbl_sch.table_name = "items";
            tbl_sch.columns = {{"id", "INT"}, {"val", "TEXT"}};
            tbl_sch.primary_key_index = 0;

            db::Row row1;
            row1.push_back(db::CellPrimitive{int64_t{10}});
            row1.push_back(db::CellPrimitive{std::string{"RecoveredValue"}});
            std::string blob1 = db::serialize_row_disk(tbl_sch, row1);
            
            // Пишем записи в WAL вручную (таблица + индекс)
            db::LogRecord rec1_tbl(0, 0, db::LogRecordType::ROW_UPSERT, 0, 
                               db::LogRecord::encodeRowPayload(table_path, key1, blob1));
            wal.appendRecord(rec1_tbl);

            db::TableSchema mini_ix;
            mini_ix.table_name = "items";
            mini_ix.columns = {{"val", "TEXT"}, {"id", "INT"}};
            mini_ix.primary_key_index = 1;

            db::Row idx_row1;
            idx_row1.push_back(db::CellPrimitive{std::string{"RecoveredValue"}});
            idx_row1.push_back(db::CellPrimitive{int64_t{10}});
            std::string idx_key1;
            db::btree_key_append_bytes(idx_key1, db::BTreeKey{idx_row1[0], idx_row1[1]});
            db::LogRecord rec1_idx(0, 0, db::LogRecordType::ROW_UPSERT, 0,
                               db::LogRecord::encodeRowPayload(index_path, idx_key1, db::serialize_row_disk(mini_ix, idx_row1)));
            wal.appendRecord(rec1_idx);

            // 2. Тест ROW_DELETE (Восстановление удаления)
            // Пишем удаление в WAL (таблица + индекс)
            db::LogRecord rec2_tbl(0, 0, db::LogRecordType::ROW_DELETE, 0,
                               db::LogRecord::encodeRowPayload(table_path, "20", ""));
            wal.appendRecord(rec2_tbl);

            std::string idx_key2 = "ToDelete" + std::string(1, '\0') + "20";
            db::LogRecord rec2_idx(0, 0, db::LogRecordType::ROW_DELETE, 0,
                               db::LogRecord::encodeRowPayload(index_path, idx_key2, ""));
            wal.appendRecord(rec2_idx);
            wal.flushTo(rec2_idx.lsn);

            // Запускаем восстановление
            wal.recover(&storage);

            // Проверяем результат
            auto rows = storage.readAllRows(db_name, "items");
            bool found_10 = false;
            bool found_20 = false;
            for (const auto& r : rows) {
                if (db::cell_to_where_string(r[0]) == "10" &&
                    db::cell_to_where_string(r[1]) == "RecoveredValue")
                    found_10 = true;
                if (db::cell_to_where_string(r[0]) == "20") found_20 = true;
            }

            // Проверка индекса (должен обновиться при логическом реплее)
            auto idx_rows = storage.indexLookup(db_name, "items", "val", "RecoveredValue");
            bool idx_10_ok = (idx_rows.size() == 1 && idx_rows[0] == "10");
            
            auto idx_rows_del = storage.indexLookup(db_name, "items", "val", "ToDelete");
            bool idx_20_del_ok = idx_rows_del.empty();

            if (found_10 && !found_20 && idx_10_ok && idx_20_del_ok) {
                std::cout << "  [OK] Logical WAL recovery (UPSERT/DELETE + Index) успешно" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] Logical WAL recovery failed: found_10=" << found_10 
                          << " found_20=" << found_20 << " idx10=" << idx_10_ok 
                          << " idx20_del=" << idx_20_del_ok << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] test_logical_wal_recovery exception: " << e.what() << std::endl;
        }
    }

    void test_cell_value_and_nulls() {
        assert_success("Создание БД для типов", "CREATE DATABASE types_db;");
        assert_success("Использование БД для типов", "USE types_db;");
        assert_success("Создание таблицы для типов", "CREATE TABLE types_test (id INT PRIMARY KEY, val TEXT, num FLOAT, b BOOL);");
        
        // 1. Тест NULL vs Empty String
        assert_success("Вставка NULL и пустой строки", 
            "INSERT INTO types_test (id, val, num, b) VALUES (1, NULL, 10.5, TRUE), (2, '', 20.0, FALSE);");
        
        assert_rows("Поиск IS NULL", "SELECT id FROM types_test WHERE val IS NULL;", 1, {{"1"}});
        assert_rows("Поиск по пустой строке", "SELECT id FROM types_test WHERE val = '';", 1, {{"2"}});
        assert_rows("NULL не равен пустой строке", "SELECT count(*) FROM types_test WHERE val = NULL;", 1, {{"0"}});

        // 2. Тест точности INT/FLOAT
        std::string large_int_str = "123456789012345678"; 
        assert_success("Вставка большого INT", 
            "INSERT INTO types_test (id, val, num) VALUES (" + large_int_str + ", 'LargeInt', 0);");
        assert_rows("Проверка точности большого INT", 
            "SELECT id FROM types_test WHERE val = 'LargeInt';", 1, {{large_int_str}});

        // 3. Тест Legacy Migration (Эмуляция v1 формата)
        total_count++;
        try {
            // Берем любую схему для теста кодека (кодек - чистая функция, это безопасно)
            db::TableSchema s;
            s.columns = {{"id", "INT"}, {"val", "TEXT"}, {"num", "FLOAT"}, {"b", "BOOL"}};
            
            // Ручно собираем v1-строку: [num_fields(2)][len(2) data][len(2) data]...
            std::string legacy_blob;
            uint16_t nf = 4;
            legacy_blob.append((char*)&nf, 2);
            
            auto add_f = [&](std::string v) {
                uint16_t l = v.size();
                legacy_blob.append((char*)&l, 2);
                legacy_blob.append(v);
            };
            add_f("500");        // id
            add_f("LegacyData"); // val
            add_f("123.456");    // num
            add_f("1");          // b
            
            db::Row recovered;
            bool ok = db::deserialize_row_disk(s, (const uint8_t*)legacy_blob.data(), legacy_blob.size(), recovered);
            
            if (ok && recovered.size() == 4 && db::cell_to_where_string(recovered[1]) == "LegacyData") {
                std::cout << "  [OK] Legacy Row Migration (v1 -> v2) успешна" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] Legacy Row Migration не сработала" << std::endl;
            }
        } catch(...) {
            std::cerr << "  [FAIL] Ошибка в тесте миграции" << std::endl;
        }

        executor.execute("DROP TABLE types_test;");
        executor.execute("DROP DATABASE types_db;");
    }

    void test_btree_typed_keys() {
        auto check = [this](const char* name, bool cond) {
            total_count++;
            if (cond) {
                std::cout << "  [OK] " << name << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] " << name << std::endl;
            }
        };

        db::TableSchema clustered;
        clustered.primary_key_index = 0;
        clustered.columns = {{"id", "INT"}};

        db::BTreeKey k_int{{db::CellPrimitive{int64_t{7}}}};
        std::string w_cluster;
        db::btree_key_append_bytes(w_cluster, k_int);
        db::BTreeKey dec_cluster;
        check("BTree wire roundtrip (clustered INT)",
              db::decode_btree_key_blob(reinterpret_cast<const uint8_t*>(w_cluster.data()),
                                        static_cast<uint32_t>(w_cluster.size()), 1, clustered,
                                        dec_cluster) &&
                  dec_cluster.size() == 1 && dec_cluster[0].has_value() &&
                  std::holds_alternative<int64_t>(*dec_cluster[0]) &&
                  std::get<int64_t>(*dec_cluster[0]) == 7);

        db::TableSchema mini;
        mini.primary_key_index = 1;
        mini.columns = {{"val", "TEXT"}, {"id", "INT"}};
        db::BTreeKey k_sec{{db::CellPrimitive{std::string{"hi"}},
                            db::CellPrimitive{int64_t{3}}}};
        std::string w_sec;
        db::btree_key_append_bytes(w_sec, k_sec);
        db::BTreeKey dec_sec;
        check("BTree wire roundtrip (secondary TEXT+INT)",
              db::decode_btree_key_blob(reinterpret_cast<const uint8_t*>(w_sec.data()),
                                        static_cast<uint32_t>(w_sec.size()), 2, mini, dec_sec) &&
                  db::compare_btree_keys(dec_sec, k_sec) == 0);

        db::BTreeKey low{{db::CellPrimitive{int64_t{1}}}};
        db::BTreeKey high{{db::CellPrimitive{int64_t{2}}}};
        check("compare_btree_keys: INT ascending",
              db::compare_btree_keys(low, high) < 0 && db::compare_btree_keys(high, low) > 0);

        db::BTreeKey a2{{db::CellPrimitive{std::string{"z"}}, db::CellPrimitive{int64_t{2}}}};
        db::BTreeKey b2{{db::CellPrimitive{std::string{"z"}}, db::CellPrimitive{int64_t{3}}}};
        check("compare_btree_keys: same indexed value, PK tie-break",
              db::compare_btree_keys(a2, b2) < 0);

        db::BTreeKey prefix{{db::CellPrimitive{int64_t{5}}}};
        db::BTreeKey full;
        full.push_back(db::CellPrimitive{int64_t{5}});
        full.push_back(db::CellPrimitive{int64_t{99}});
        check("compare_btree_keys_nav: prefix vs composite (equal for navigation)",
              db::compare_btree_keys_nav(prefix, full) == 0 &&
                  db::compare_btree_keys_nav(full, prefix) == 0);

        std::string leg_pk = "99";
        db::BTreeKey leg_dec;
        check("Legacy clustered key decode (lexical INT)",
              db::btree_key_from_legacy_bytes(reinterpret_cast<const uint8_t*>(leg_pk.data()),
                                              leg_pk.size(), 1, clustered, leg_dec) &&
                  leg_dec.size() == 1 && std::get<int64_t>(*leg_dec[0]) == 99);

        std::string leg_ix = std::string("foo") + std::string(1, '\0') + "42";
        db::BTreeKey leg_ix_dec;
        check("Legacy secondary key decode (col\\0pk)",
              db::btree_key_from_legacy_bytes(reinterpret_cast<const uint8_t*>(leg_ix.data()),
                                              leg_ix.size(), 2, mini, leg_ix_dec) &&
                  std::get<std::string>(*leg_ix_dec[0]) == "foo" &&
                  std::get<int64_t>(*leg_ix_dec[1]) == 42);

        std::string w_bad = w_cluster;
        w_bad.push_back('\x01');
        db::BTreeKey junk;
        check("btree_key_parts_from_bytes rejects trailing garbage",
              !db::btree_key_parts_from_bytes(reinterpret_cast<const uint8_t*>(w_bad.data()),
                                              w_bad.size(), 1, junk));

        check("compare_cell_values: NULL sorts before non-NULL",
              db::compare_cell_values(std::nullopt, db::CellPrimitive{int64_t{0}}) < 0 &&
                  db::compare_cell_values(db::CellPrimitive{int64_t{0}}, std::nullopt) > 0);
    }

    void test_load_csv() {
        try {
            const std::string db_name = "csv_load_db";
            const std::string csv_name = "batch_import.csv";
            std::filesystem::path csv_path = std::filesystem::path(data_dir) / db_name / csv_name;

            assert_success("Создание БД для LOAD CSV", "CREATE DATABASE " + db_name + ";");
            assert_success("USE для LOAD CSV", "USE " + db_name + ";");
            assert_success("Таблица для LOAD CSV",
                             "CREATE TABLE csv_t (id INT PRIMARY KEY, name TEXT, age INT);");

            std::filesystem::create_directories(csv_path.parent_path());
            {
                std::ofstream out(csv_path);
                out << "age,id,name\n";
                out << "40,100,Zeta\n";
                out << "22,101,\"Quoted, Name\"\n";
            }

            json res = executor.execute("LOAD CSV '" + csv_name + "' INTO csv_t;");
            bool ok_load = res["success"].get<bool>();

            json sel = executor.execute("SELECT id, name FROM csv_t ORDER BY id;");
            bool rows_ok = false;
            if (sel["success"].get<bool>() && sel.contains("rows") && sel["rows"].is_array() &&
                sel["rows"].size() == 2) {
                rows_ok = (sel["rows"][0][0].get<std::string>() == "100" &&
                            sel["rows"][0][1].get<std::string>() == "Zeta" &&
                            sel["rows"][1][0].get<std::string>() == "101" &&
                            sel["rows"][1][1].get<std::string>() == "Quoted, Name");
            }

            assert_success("Очистка LOAD CSV", "DROP TABLE csv_t; DROP DATABASE " + db_name + ";");
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(data_dir) / db_name, ec);

            total_count++;
            if (ok_load && rows_ok) {
                std::cout << "  [OK] LOAD CSV (relative path, header order, quoted field)" << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] LOAD CSV: ok=" << ok_load << " res=" << res.dump()
                          << " sel=" << sel.dump() << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] test_load_csv exception: " << e.what() << std::endl;
        }
    }

    void test_load_csv_nonempty_and_append() {
        try {
            const std::string db_name = "csv_append_db";
            const std::string csv_name = "more_rows.csv";
            std::filesystem::path csv_path = std::filesystem::path(data_dir) / db_name / csv_name;

            assert_success("БД для LOAD CSV APPEND", "CREATE DATABASE " + db_name + ";");
            assert_success("USE csv_append_db", "USE " + db_name + ";");
            assert_success("Таблица для APPEND",
                           "CREATE TABLE csv_append_t (id INT PRIMARY KEY, name TEXT, age INT);");
            assert_success("Первая строка до CSV", "INSERT INTO csv_append_t (id, name, age) VALUES (1, 'First', 99);");

            std::filesystem::create_directories(csv_path.parent_path());
            {
                std::ofstream out(csv_path);
                out << "name,age,id\n";
                out << "Second,20,2\n";
            }

            json reject = executor.execute("LOAD CSV '" + csv_name + "' INTO csv_append_t;");
            bool rejected = !reject["success"].get<bool>();
            std::string msg = reject.value("message", "");
            bool msg_ok = msg.find("not empty") != std::string::npos || msg.find("APPEND") != std::string::npos;

            json ok_append =
                executor.execute("LOAD CSV '" + csv_name + "' INTO csv_append_t APPEND;");
            bool append_ok = ok_append["success"].get<bool>();

            json cnt = executor.execute("SELECT COUNT(*) FROM csv_append_t;");
            bool count_ok = false;
            if (cnt["success"].get<bool>() && cnt.contains("rows") && cnt["rows"].is_array() &&
                !cnt["rows"].empty()) {
                count_ok = (cnt["rows"][0][0].get<std::string>() == "2");
            }

            assert_success("Очистка APPEND-теста",
                           "DROP TABLE csv_append_t; DROP DATABASE " + db_name + ";");
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(data_dir) / db_name, ec);

            total_count++;
            if (rejected && msg_ok && append_ok && count_ok) {
                std::cout << "  [OK] LOAD CSV: отказ если таблица не пуста; APPEND добавляет строки"
                          << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] LOAD CSV APPEND: rejected=" << rejected << " msg_ok=" << msg_ok
                          << " append_ok=" << append_ok << " count_ok=" << count_ok << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] test_load_csv_nonempty_and_append: " << e.what() << std::endl;
        }
    }

    void test_alter_add_column_from_csv() {
        try {
            const std::string db_name = "alter_csv_db";
            const std::string csv_name = "scores.csv";
            std::filesystem::path csv_path = std::filesystem::path(data_dir) / db_name / csv_name;

            assert_success("БД для ALTER CSV", "CREATE DATABASE " + db_name + ";");
            assert_success("USE alter_csv_db", "USE " + db_name + ";");
            assert_success("Таблица users_alter",
                           "CREATE TABLE users_alter (id INT PRIMARY KEY, name TEXT);");
            assert_success("Две строки", "INSERT INTO users_alter (id, name) VALUES (1, 'A'), (2, 'B');");

            std::filesystem::create_directories(csv_path.parent_path());
            {
                std::ofstream out(csv_path);
                out << "score,id\n";
                out << "100,1\n";
                out << "200,2\n";
            }

            json alt = executor.execute(
                "ALTER TABLE users_alter ADD COLUMN score INT FROM CSV '" + csv_name + "';");
            bool alt_ok = alt["success"].get<bool>();

            json sel = executor.execute("SELECT id, name, score FROM users_alter ORDER BY id;");
            bool data_ok = false;
            if (sel["success"].get<bool>() && sel.contains("rows") && sel["rows"].is_array() &&
                sel["rows"].size() == 2) {
                data_ok = (sel["rows"][0][0].get<std::string>() == "1" &&
                            sel["rows"][0][2].get<std::string>() == "100" &&
                            sel["rows"][1][0].get<std::string>() == "2" &&
                            sel["rows"][1][2].get<std::string>() == "200");
            }

            json bad = executor.execute(
                "ALTER TABLE users_alter ADD COLUMN extra INT FROM CSV '" + csv_name + "';");
            bool bad_rejected = !bad["success"].get<bool>();

            assert_success("Очистка ALTER CSV",
                           "DROP TABLE users_alter; DROP DATABASE " + db_name + ";");
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(data_dir) / db_name, ec);

            total_count++;
            if (alt_ok && data_ok && bad_rejected) {
                std::cout << "  [OK] ALTER TABLE ADD COLUMN ... FROM CSV (полное покрытие PK)"
                          << std::endl;
                passed_count++;
            } else {
                std::cerr << "  [FAIL] ALTER FROM CSV: alt_ok=" << alt_ok << " data_ok=" << data_ok
                          << " bad_rejected=" << bad_rejected << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "  [FAIL] test_alter_add_column_from_csv: " << e.what() << std::endl;
        }
    }
    void test_multi_line_and_aggr_aliases() {
        assert_success("Создание базы для многострочных тестов", "CREATE DATABASE multi_db;");
        assert_success("Использование multi_db", "USE multi_db;");
        assert_success("Создание таблицы продаж", 
            "CREATE TABLE sales (\n"
            "  id INT PRIMARY KEY,\n"
            "  category_id INT,\n"
            "  price FLOAT\n"
            ");");
        
        assert_success("Многострочная вставка", 
            "INSERT INTO sales (id, category_id, price) \n"
            "VALUES (1, 10, 100.5), (2, 10, 200.0), (3, 20, 50.0);");

        // Тест на использование 'count' как алиаса и в HAVING
        assert_rows("Агрегаты с алиасом 'count' и HAVING", 
            "SELECT category_id, COUNT(*) as count, AVG(price) as avg_price \n"
            "FROM sales \n"
            "GROUP BY category_id \n"
            "HAVING count > 1;", 
            1, {{"10", "2", "150.250"}});

        // Тест на использование других ключевых слов как алиасов
        assert_rows("Использование других KW как алиасов",
            "SELECT category_id AS index, SUM(price) AS sum FROM sales GROUP BY category_id ORDER BY sum DESC LIMIT 1;",
            1, {{"10", "300.500"}});

        assert_success("Очистка multi_db", "DROP DATABASE multi_db;");
    }
    void test_rbac() {
        std::cout << "\n>>> ФАЗА 3: Ролевая модель доступа (RBAC)" << std::endl;

        // 1. Подготовка (от имени админа)
        assert_success("RBAC: Создание базы", "CREATE DATABASE rbac_db;");
        assert_success("RBAC: Выбор базы", "USE rbac_db;");
        assert_success("RBAC: Настройка контекста админа", "SET USER admin;");
        assert_success("RBAC: Создание таблицы", "CREATE TABLE vault (id INT PRIMARY KEY, secret_data VARCHAR(100));");
        assert_success("RBAC: Вставка базовых данных", "INSERT INTO vault VALUES (1, 'Top Secret');");

        // 2. Создание аккаунтов и ролей
        assert_success("RBAC: Создание пользователей", "CREATE USER alice PASSWORD 'pass_a';");
        assert_success("RBAC: Создание пользователей", "CREATE USER bob PASSWORD 'pass_b';");
        assert_success("RBAC: Создание пользователей", "CREATE USER eve PASSWORD 'pass_e';"); // Хакер
        
        assert_success("RBAC: Создание ролей", "CREATE ROLE reader;");
        assert_success("RBAC: Создание ролей", "CREATE ROLE writer;");

        // 3. Выдача разрешений (Гранты)
        assert_success("RBAC: Права на чтение", "GRANT SELECT ON vault TO reader;");
        assert_success("RBAC: Права на запись", "GRANT INSERT ON vault TO writer;");
        assert_success("RBAC: Права на обновление", "GRANT UPDATE ON vault TO writer;");
        
        assert_success("RBAC: Назначение ролей", "GRANT ROLE reader TO alice;");
        assert_success("RBAC: Назначение ролей", "GRANT ROLE reader TO bob;"); // Боб может и читать
        assert_success("RBAC: Назначение ролей", "GRANT ROLE writer TO bob;"); // ...и писать

        // 4. Тестирование Аутентификации
        assert_error("RBAC: Неверный юзер", "SET USER ghost PASSWORD '123';", "does not exist");
        assert_error("RBAC: Неверный пароль", "SET USER alice PASSWORD 'wrong';", "Invalid password");

        // 5. Тестирование Изоляции: ALICE (Только чтение)
        assert_success("RBAC: Авторизация Alice", "SET USER alice PASSWORD 'pass_a';");
        assert_rows("RBAC: Alice читает", "SELECT * FROM vault;", 1, {{"1", "Top Secret"}});
        assert_error("RBAC: Alice пытается писать", "INSERT INTO vault VALUES (2, 'Alice data');", "Permission denied");
        assert_error("RBAC: Alice пытается удалять", "DELETE FROM vault WHERE id = 1;", "Permission denied");
        assert_error("RBAC: Alice пытается создать таблицу", "CREATE TABLE backdoor (id INT);", "Permission denied");

        // 6. Тестирование Прав: BOB (Чтение и Запись)
        assert_success("RBAC: Авторизация Bob", "SET USER bob PASSWORD 'pass_b';");
        assert_success("RBAC: Bob пишет", "INSERT INTO vault VALUES (2, 'Bob data');");
        assert_success("RBAC: Bob обновляет", "UPDATE vault SET secret_data = 'Updated' WHERE id = 1;");
        assert_rows("RBAC: Bob читает изменения", "SELECT id FROM vault WHERE id = 2;", 1, {{"2"}});
        // У Боба нет прав на DROP
        assert_error("RBAC: Bob пытается удалить таблицу", "DROP TABLE vault;", "Permission denied");

        // 7. Тестирование Безопасности: EVE (Без ролей)
        assert_success("RBAC: Авторизация Eve", "SET USER eve PASSWORD 'pass_e';");
        assert_error("RBAC: Eve пытается читать", "SELECT * FROM vault;", "Permission denied");
        assert_error("RBAC: Eve пытается писать", "INSERT INTO vault VALUES (3, 'Malware');", "Permission denied");
        
        // 8. Защита системного каталога (Критически важно!)
        assert_error("RBAC: Eve атакует sys_users", "INSERT INTO sys_users VALUES (99, 'root', 'pwd');", "Permission denied");
        assert_error("RBAC: Eve атакует sys_grants", "INSERT INTO sys_grants VALUES (99, 1, '*', 'ALL');", "Permission denied");

        // 9. Очистка и возврат к админу
        assert_success("RBAC: Авторизация Admin", "SET USER admin;");
        assert_success("RBAC: Админ удаляет базу", "DROP DATABASE rbac_db;");
    }
};

int main() {
    TestSuite suite("test_db_data_for_executor_tests");
    suite.run_all();
    return 0;
}
