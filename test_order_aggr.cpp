#include "engine/executor.hpp"
#include <iostream>
int main() {
    db::Executor exec("data");
    // Cleanup
    exec.execute("DROP DATABASE test_oa;");
    std::cout << exec.execute("CREATE DATABASE test_oa;").dump() << std::endl;
    std::cout << exec.execute("USE test_oa;").dump() << std::endl;
    std::cout << exec.execute("CREATE TABLE workers(id INT PRIMARY KEY, name TEXT, salary FLOAT, department TEXT);").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (1, 'Alice', 100.5, 'HR');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (2, 'Bob', 200.0, 'IT');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (3, 'Charlie', 300.5, 'IT');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (4, 'David', 150.0, 'HR');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (5, 'Eve', 400.0, 'IT');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO workers VALUES (6, 'Frank', 50.0, 'Sales');").dump() << std::endl;

    // The exact query that fails
    std::string q = "SELECT department, COUNT(*), SUM(salary), AVG(salary) FROM workers GROUP BY department HAVING SUM(salary) > 200 ORDER BY AVG(salary) DESC;";
    std::cout << "Query: " << q << std::endl;
    std::cout << exec.execute(q).dump(2) << std::endl;
    return 0;
}
