#include "engine/executor.hpp"
#include <iostream>
int main() {
    db::Executor exec("data");
    std::cout << exec.execute("CREATE DATABASE testexec;").dump() << std::endl;
    std::cout << exec.execute("USE testexec;").dump() << std::endl;
    std::cout << exec.execute("CREATE TABLE emp(id INT PRIMARY KEY, name TEXT, salary FLOAT, dept TEXT);").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO emp VALUES (1, 'Alice', 100.5, 'HR');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO emp VALUES (2, 'Bob', 200.0, 'ENG');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO emp VALUES (3, 'Charlie', 300.5, 'ENG');").dump() << std::endl;
    std::cout << exec.execute("INSERT INTO emp VALUES (4, 'David', 150.0, 'HR');").dump() << std::endl;
    std::cout << exec.execute("SELECT COUNT(*), dept, SUM(salary) FROM emp GROUP BY dept HAVING SUM(salary) > 200 ORDER BY AVG(salary) DESC;").dump() << std::endl;
    return 0;
}
