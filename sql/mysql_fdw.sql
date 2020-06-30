\c postgres postgres
CREATE EXTENSION mysql_fdw;
CREATE SERVER mysql_svr FOREIGN DATA WRAPPER mysql_fdw;
CREATE USER MAPPING FOR postgres SERVER mysql_svr OPTIONS(username 'foo', password 'bar');

CREATE FOREIGN TABLE department(department_id int, department_name text) SERVER mysql_svr OPTIONS(dbname 'testdb', table_name 'department');
CREATE FOREIGN TABLE employee(emp_id int, emp_name text, emp_dept_id int) SERVER mysql_svr OPTIONS(dbname 'testdb', table_name 'employee');
CREATE FOREIGN TABLE empdata(emp_id int, emp_dat bytea) SERVER mysql_svr OPTIONS(dbname 'testdb', table_name 'empdata');
CREATE FOREIGN TABLE numbers(a int, b varchar(255)) SERVER mysql_svr OPTIONS (dbname 'testdb', table_name 'numbers');

--Testcase 1:
SELECT * FROM department LIMIT 10;
--Testcase 2:
SELECT * FROM employee LIMIT 10;
--Testcase 3:
SELECT * FROM empdata LIMIT 10;

--Testcase 4:
INSERT INTO department VALUES(generate_series(1,100), 'dept - ' || generate_series(1,100));
--Testcase 5:
INSERT INTO employee VALUES(generate_series(1,100), 'emp - ' || generate_series(1,100), generate_series(1,100));
--Testcase 6:
INSERT INTO empdata  VALUES(1, decode ('01234567', 'hex'));

--Testcase 7:
insert into numbers values(1, 'One');
--Testcase 8:
insert into numbers values(2, 'Two');
--Testcase 9:
insert into numbers values(3, 'Three');
--Testcase 10:
insert into numbers values(4, 'Four');
--Testcase 11:
insert into numbers values(5, 'Five');
--Testcase 12:
insert into numbers values(6, 'Six');
--Testcase 13:
insert into numbers values(7, 'Seven');
--Testcase 14:
insert into numbers values(8, 'Eight');
--Testcase 15:
insert into numbers values(9, 'Nine');

--Testcase 16:
SELECT count(*) FROM department;
--Testcase 17:
SELECT count(*) FROM employee;
--Testcase 18:
SELECT count(*) FROM empdata;

--Testcase 19:
EXPLAIN (COSTS FALSE) SELECT * FROM department d, employee e WHERE d.department_id = e.emp_dept_id LIMIT 10;

--Testcase 20:
EXPLAIN (COSTS FALSE) SELECT * FROM department d, employee e WHERE d.department_id IN (SELECT department_id FROM department) LIMIT 10;

--Testcase 21:
SELECT * FROM department d, employee e WHERE d.department_id = e.emp_dept_id LIMIT 10;
--Testcase 22:
SELECT * FROM department d, employee e WHERE d.department_id IN (SELECT department_id FROM department) LIMIT 10;
--Testcase 23:
SELECT * FROM empdata;

--Testcase 24:
DELETE FROM employee WHERE emp_id = 10;

--Testcase 25:
SELECT COUNT(*) FROM department LIMIT 10;
--Testcase 26:
SELECT COUNT(*) FROM employee WHERE emp_id = 10;

--Testcase 27:
UPDATE employee SET emp_name = 'Updated emp' WHERE emp_id = 20;
--Testcase 28:
SELECT emp_id, emp_name FROM employee WHERE emp_name like 'Updated emp';

--Testcase 29:
UPDATE empdata SET emp_dat = decode ('0123', 'hex');
--Testcase 30:
SELECT * FROM empdata;

--Testcase 31:
SELECT * FROM employee LIMIT 10;
--Testcase 32:
SELECT * FROM employee WHERE emp_id IN (1);
--Testcase 33:
SELECT * FROM employee WHERE emp_id IN (1,3,4,5);
--Testcase 34:
SELECT * FROM employee WHERE emp_id IN (10000,1000);

--Testcase 35:
SELECT * FROM employee WHERE emp_id NOT IN (1) LIMIT 5;
--Testcase 36:
SELECT * FROM employee WHERE emp_id NOT IN (1,3,4,5) LIMIT 5;
--Testcase 37:
SELECT * FROM employee WHERE emp_id NOT IN (10000,1000) LIMIT 5;

--Testcase 38:
SELECT * FROM employee WHERE emp_id NOT IN (SELECT emp_id FROM employee WHERE emp_id IN (1,10));
--Testcase 39:
SELECT * FROM employee WHERE emp_name NOT IN ('emp - 1', 'emp - 2') LIMIT 5;
--Testcase 40:
SELECT * FROM employee WHERE emp_name NOT IN ('emp - 10') LIMIT 5;

create or replace function test_param_where() returns void as $$
DECLARE
  n varchar;
BEGIN
  FOR x IN 1..9 LOOP
    select b into n from numbers where a=x;
    raise notice 'Found number %', n;
  end loop;
  return;
END
$$ LANGUAGE plpgsql;

--Testcase 41:
SELECT test_param_where();

create or replace function test_param_where2(integer, text) returns integer as '
  select a from numbers where a=$1 and b=$2;
' LANGUAGE sql;

--Testcase 42:
SELECT test_param_where2(1, 'One');

--Testcase 43:
DELETE FROM employee;
--Testcase 44:
DELETE FROM department;
--Testcase 45:
DELETE FROM empdata;
--Testcase 46:
DELETE FROM numbers;

DROP FUNCTION test_param_where();
DROP FUNCTION test_param_where2(integer, text);
DROP FOREIGN TABLE numbers;

DROP FOREIGN TABLE department;
DROP FOREIGN TABLE employee;
DROP FOREIGN TABLE empdata;
DROP USER MAPPING FOR postgres SERVER mysql_svr;
DROP SERVER mysql_svr;
DROP EXTENSION mysql_fdw CASCADE;
