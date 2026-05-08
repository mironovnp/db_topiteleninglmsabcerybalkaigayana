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

    void test_ddl_and_errors() {
        assert_error("Запрос без базы данных", "CREATE TABLE t (id INT);", "No database selected");
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
};

int main() {
    TestSuite suite("test_db_data_for_executor_tests");
    suite.run_all();
    return 0;
}
