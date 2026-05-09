#include "engine/executor.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
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
};

int main() {
    TestSuite suite("test_db_data_for_executor_tests");
    suite.run_all();
    return 0;
}
