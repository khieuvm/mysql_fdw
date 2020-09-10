\set MYSQL_HOST			'\'localhost\''
\set MYSQL_PORT			'\'3306\''
\set MYSQL_USER_NAME	'\'edb\''
\set MYSQL_PASS			'\'edb\''

-- Before running this file User must create database mysql_fdw_regress on
-- mysql with all permission for 'edb' user with 'edb' password and ran
-- mysql_init.sh file to create tables.

\c contrib_regression
--Testcase 1:
CREATE EXTENSION IF NOT EXISTS mysql_fdw;
--Testcase 2:
CREATE SERVER mysql_svr FOREIGN DATA WRAPPER mysql_fdw
  OPTIONS (host :MYSQL_HOST, port :MYSQL_PORT);
--Testcase 3:
CREATE USER MAPPING FOR public SERVER mysql_svr
  OPTIONS (username :MYSQL_USER_NAME, password :MYSQL_PASS);

-- Create foreign tables
--Testcase 4:
CREATE FOREIGN TABLE f_test_tbl1 (c1 INTEGER, c2 VARCHAR(10), c3 CHAR(9), c4 BIGINT, c5 pg_catalog.Date, c6 DECIMAL, c7 INTEGER, c8 SMALLINT)
  SERVER mysql_svr OPTIONS (dbname 'mysql_fdw_regress', table_name 'test_tbl1');
--Testcase 5:
CREATE FOREIGN TABLE f_test_tbl2 (c1 INTEGER, c2 VARCHAR(14), c3 VARCHAR(13))
  SERVER mysql_svr OPTIONS (dbname 'mysql_fdw_regress', table_name 'test_tbl2');

-- Insert data in mysql db using foreign tables
--Testcase 6:
INSERT INTO f_test_tbl1 VALUES (100, 'EMP1', 'ADMIN', 1300, '1980-12-17', 800.23, NULL, 20);
--Testcase 7:
INSERT INTO f_test_tbl1 VALUES (200, 'EMP2', 'SALESMAN', 600, '1981-02-20', 1600.00, 300, 30);
--Testcase 8:
INSERT INTO f_test_tbl1 VALUES (300, 'EMP3', 'SALESMAN', 600, '1981-02-22', 1250, 500, 30);
--Testcase 9:
INSERT INTO f_test_tbl1 VALUES (400, 'EMP4', 'MANAGER', 900, '1981-04-02', 2975.12, NULL, 20);
--Testcase 10:
INSERT INTO f_test_tbl1 VALUES (500, 'EMP5', 'SALESMAN', 600, '1981-09-28', 1250, 1400, 30);
--Testcase 11:
INSERT INTO f_test_tbl1 VALUES (600, 'EMP6', 'MANAGER', 900, '1981-05-01', 2850, NULL, 30);
--Testcase 12:
INSERT INTO f_test_tbl1 VALUES (700, 'EMP7', 'MANAGER', 900, '1981-06-09', 2450.45, NULL, 10);
--Testcase 13:
INSERT INTO f_test_tbl1 VALUES (800, 'EMP8', 'FINANCE', 400, '1987-04-19', 3000, NULL, 20);
--Testcase 14:
INSERT INTO f_test_tbl1 VALUES (900, 'EMP9', 'HEAD', NULL, '1981-11-17', 5000, NULL, 10);
--Testcase 15:
INSERT INTO f_test_tbl1 VALUES (1000, 'EMP10', 'SALESMAN', 600, '1980-09-08', 1500, 0, 30);
--Testcase 16:
INSERT INTO f_test_tbl1 VALUES (1100, 'EMP11', 'ADMIN', 800, '1987-05-23', 1100, NULL, 20);
--Testcase 17:
INSERT INTO f_test_tbl1 VALUES (1200, 'EMP12', 'ADMIN', 600, '1981-12-03', 950, NULL, 30);
--Testcase 18:
INSERT INTO f_test_tbl1 VALUES (1300, 'EMP13', 'FINANCE', 400, '1981-12-03', 3000, NULL, 20);
--Testcase 19:
INSERT INTO f_test_tbl1 VALUES (1400, 'EMP14', 'ADMIN', 700, '1982-01-23', 1300, NULL, 10);
--Testcase 20:
INSERT INTO f_test_tbl2 VALUES(10, 'DEVELOPMENT', 'PUNE');
--Testcase 21:
INSERT INTO f_test_tbl2 VALUES(20, 'ADMINISTRATION', 'BANGLORE');
--Testcase 22:
INSERT INTO f_test_tbl2 VALUES(30, 'SALES', 'MUMBAI');
--Testcase 23:
INSERT INTO f_test_tbl2 VALUES(40, 'HR', 'NAGPUR');

SET datestyle TO ISO;

-- WHERE clause pushdown

--Testcase 24:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6 AS "salary", c8 FROM f_test_tbl1 e
  WHERE c6 IN (800,2450)
  ORDER BY c1;
--Testcase 25:
SELECT c1, c2, c6 AS "salary", c8 FROM f_test_tbl1 e
  WHERE c6 IN (800,2450)
  ORDER BY c1;

--Testcase 26:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT * FROM f_test_tbl1 e
  WHERE c6 > 3000
  ORDER BY c1;
--Testcase 27:
SELECT * FROM f_test_tbl1 e
  WHERE c6 > 3000
  ORDER BY c1;

--Testcase 28:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 = 1500
  ORDER BY c1;
--Testcase 29:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 = 1500
  ORDER BY c1;

--Testcase 30:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 BETWEEN 1000 AND 4000
  ORDER BY c1;
--Testcase 31:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 BETWEEN 1000 AND 4000
  ORDER BY c1;

--Testcase 32:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IS NOT NULL
  ORDER BY c1;
--Testcase 33:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IS NOT NULL
  ORDER BY c1;

--Testcase 34:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT * FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17'
  ORDER BY c1;
--Testcase 35:
SELECT * FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17'
  ORDER BY c1;

--Testcase 36:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;
--Testcase 37:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;

--Testcase 38:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;
--Testcase 39:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;

--Testcase 40:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'SALESMAN'
  ORDER BY c1;
--Testcase 41:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'SALESMAN'
  ORDER BY c1;

--Testcase 42:
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'MANA%'
  ORDER BY c1;
--Testcase 43:
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'MANA%'
  ORDER BY c1;

-- Cleanup
--Testcase 44:
DELETE FROM f_test_tbl1;
--Testcase 45:
DELETE FROM f_test_tbl2;
--Testcase 46:
DROP FOREIGN TABLE f_test_tbl1;
--Testcase 47:
DROP FOREIGN TABLE f_test_tbl2;
--Testcase 48:
DROP USER MAPPING FOR public SERVER mysql_svr;
--Testcase 49:
DROP SERVER mysql_svr;
--Testcase 50:
DROP EXTENSION mysql_fdw;
