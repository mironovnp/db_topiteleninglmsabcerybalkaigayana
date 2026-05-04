CREATE DATABASE test2;
USE test2;
CREATE TABLE depts (id INT PRIMARY KEY, name VARCHAR(50) UNIQUE NOT NULL);
INSERT INTO depts (id, name) VALUES (1, 'Engineering');
INSERT INTO depts (id, name) VALUES (2, 'HR');
INSERT INTO depts (id, name) VALUES (3, 'Sales');
SELECT name FROM depts ORDER BY id DESC LIMIT 2 OFFSET 1;
