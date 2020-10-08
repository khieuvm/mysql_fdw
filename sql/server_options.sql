\set MYSQL_HOST			'\'localhost\''
\set MYSQL_PORT			'\'3306\''
\set MYSQL_USER_NAME	'\'edb\''
\set MYSQL_PASS			'\'edb\''

-- Before running this file User must create database mysql_fdw_regress on
-- MySQL with all permission for 'edb' user with 'edb' password and ran
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

-- Validate extension, server and mapping details
--Testcase 4:
SELECT e.fdwname as "Extension", srvname AS "Server", s.srvoptions AS "Server_Options", u.umoptions AS "User_Mapping_Options"
  FROM pg_foreign_data_wrapper e LEFT JOIN pg_foreign_server s ON e.oid = s.srvfdw LEFT JOIN pg_user_mapping u ON s.oid = u.umserver
  WHERE e.fdwname = 'mysql_fdw'
  ORDER BY 1, 2, 3, 4;

-- Create foreign table and perform basic SQL operations
--Testcase 5:
CREATE FOREIGN TABLE f_mysql_test(a int, b int)
  SERVER mysql_svr OPTIONS (dbname 'mysql_fdw_regress', table_name 'mysql_test');
--Testcase 6:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;
--Testcase 7:
INSERT INTO f_mysql_test (a, b) VALUES (2, 2);
--Testcase 8:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;
--Testcase 9:
UPDATE f_mysql_test SET b = 3 WHERE a = 2;
--Testcase 10:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;
--Testcase 11:
DELETE FROM f_mysql_test WHERE a = 2;
--Testcase 12:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;

--Testcase 13:
DROP FOREIGN TABLE f_mysql_test;
--Testcase 14:
DROP USER MAPPING FOR public SERVER mysql_svr;
--Testcase 15:
DROP SERVER mysql_svr;

-- Server with init_command.
--Testcase 16:
CREATE SERVER mysql_svr1 FOREIGN DATA WRAPPER mysql_fdw
  OPTIONS (host :MYSQL_HOST, port :MYSQL_PORT, init_command 'create table init_command_check(a int)');
--Testcase 17:
CREATE USER MAPPING FOR public SERVER mysql_svr1
  OPTIONS (username :MYSQL_USER_NAME, password :MYSQL_PASS);
--Testcase 18:
CREATE FOREIGN TABLE f_mysql_test (a int, b int)
  SERVER mysql_svr1 OPTIONS (dbname 'mysql_fdw_regress', table_name 'mysql_test');
-- This will create init_command_check table in mysql_fdw_regress database.
--Testcase 19:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;

-- init_command_check table created mysql_fdw_regress database can be verified
-- by creating corresponding foreign table here.
--Testcase 20:
CREATE FOREIGN TABLE f_init_command_check(a int)
  SERVER mysql_svr1 OPTIONS (dbname 'mysql_fdw_regress', table_name 'init_command_check');
--Testcase 21:
SELECT a FROM f_init_command_check ORDER BY 1;
-- Changing init_command to drop init_command_check table from
-- mysql_fdw_regress database
ALTER SERVER mysql_svr1 OPTIONS (SET init_command 'drop table init_command_check');
--Testcase 22:
SELECT a, b FROM f_mysql_test;

--Testcase 23:
DROP FOREIGN TABLE f_init_command_check;
--Testcase 24:
DROP FOREIGN TABLE f_mysql_test;
--Testcase 25:
DROP USER MAPPING FOR public SERVER mysql_svr1;
--Testcase 26:
DROP SERVER mysql_svr1;

-- Server with use_remote_estimate.
--Testcase 27:
CREATE SERVER mysql_svr1 FOREIGN DATA WRAPPER mysql_fdw
  OPTIONS(host :MYSQL_HOST, port :MYSQL_PORT, use_remote_estimate 'TRUE');
--Testcase 28:
CREATE USER MAPPING FOR public SERVER mysql_svr1
  OPTIONS(username :MYSQL_USER_NAME, password :MYSQL_PASS);
--Testcase 29:
CREATE FOREIGN TABLE f_mysql_test(a int, b int)
  SERVER mysql_svr1 OPTIONS(dbname 'mysql_fdw_regress', table_name 'mysql_test');

-- Below explain will return actual rows from MySQL, but keeping costs off
-- here for consistent regression result.
--Testcase 30:
EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM f_mysql_test WHERE a < 2 ORDER BY 1;

--Testcase 31:
DROP FOREIGN TABLE f_mysql_test;
--Testcase 32:
DROP USER MAPPING FOR public SERVER mysql_svr1;
--Testcase 33:
DROP SERVER mysql_svr1;

-- Create server with secure_auth.
--Testcase 34:
CREATE SERVER mysql_svr1 FOREIGN DATA WRAPPER mysql_fdw
  OPTIONS(host :MYSQL_HOST, port :MYSQL_PORT, secure_auth 'FALSE');
--Testcase 35:
CREATE USER MAPPING FOR public SERVER mysql_svr1
  OPTIONS(username :MYSQL_USER_NAME, password :MYSQL_PASS);
--Testcase 36:
CREATE FOREIGN TABLE f_mysql_test(a int, b int)
  SERVER mysql_svr1 OPTIONS(dbname 'mysql_fdw_regress', table_name 'mysql_test');

-- Below should fail with Warning of secure_auth is false.
--Testcase 37:
SELECT a, b FROM f_mysql_test ORDER BY 1, 2;
--Testcase 38:
DROP FOREIGN TABLE f_mysql_test;
--Testcase 39:
DROP USER MAPPING FOR public SERVER mysql_svr1;
--Testcase 40:
DROP SERVER mysql_svr1;

-- Cleanup
--Testcase 41:
DROP EXTENSION mysql_fdw;
