SET datestyle=ISO;
SET timezone='Japan';

--Testcase 1:
CREATE EXTENSION mysql_fdw;
--Testcase 2:
CREATE SERVER server1 FOREIGN DATA WRAPPER mysql_fdw;
--Testcase 3:
CREATE USER MAPPING FOR CURRENT_USER SERVER server1 OPTIONS(username 'edb', password 'edb');

--IMPORT FOREIGN SCHEMA public FROM SERVER server1 INTO public OPTIONS(import_time_text 'false');
--Testcase 4:
CREATE FOREIGN TABLE s3(id int, tag1 text, value1 float, value2 int, value3 float, value4 int, str1 text, str2 text) SERVER server1 OPTIONS(dbname 'mysql_fdw_regress', table_name 's3');

-- s3 (value1 as float8, value2 as bigint)
--Testcase 5:
\d s3;
--Testcase 6:
SELECT * FROM s3;

-- select float8() (not pushdown, remove float8, explain)
--Testcase 7:
EXPLAIN VERBOSE
SELECT float8(value1), float8(value2), float8(value3), float8(value4) FROM s3;

-- select float8() (not pushdown, remove float8, result)
--Testcase 8:
SELECT float8(value1), float8(value2), float8(value3), float8(value4) FROM s3;

-- select sqrt (builtin function, explain)
--Testcase 9:
EXPLAIN VERBOSE
SELECT sqrt(value1), sqrt(value2) FROM s3;

-- select sqrt (buitin function, result)
--Testcase 10:
SELECT sqrt(value1), sqrt(value2) FROM s3;

-- select sqrt (builtin function,, not pushdown constraints, explain)
--Testcase 11:
EXPLAIN VERBOSE
SELECT sqrt(value1), sqrt(value2) FROM s3 WHERE to_hex(value2) != '64';

-- select sqrt (builtin function, not pushdown constraints, result)
--Testcase 12:
SELECT sqrt(value1), sqrt(value2) FROM s3 WHERE to_hex(value2) != '64';

-- select sqrt (builtin function, pushdown constraints, explain)
--Testcase 13:
EXPLAIN VERBOSE
SELECT sqrt(value1), sqrt(value2) FROM s3 WHERE value2 != 200;

-- select sqrt (builtin function, pushdown constraints, result)
--Testcase 14:
SELECT sqrt(value1), sqrt(value2) FROM s3 WHERE value2 != 200;

-- select abs (builtin function, explain)
--Testcase 15:
EXPLAIN VERBOSE
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3;

-- ABS() returns negative values if integer (https://github.com/influxdata/influxdb/issues/10261)
-- select abs (buitin function, result)
--Testcase 16:
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3;

-- select abs (builtin function, not pushdown constraints, explain)
--Testcase 17:
EXPLAIN VERBOSE
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3 WHERE to_hex(value2) != '64';

-- select abs (builtin function, not pushdown constraints, result)
--Testcase 18:
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3 WHERE to_hex(value2) != '64';

-- select abs (builtin function, pushdown constraints, explain)
--Testcase 19:
EXPLAIN VERBOSE
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3 WHERE value2 != 200;

-- select abs (builtin function, pushdown constraints, result)
--Testcase 20:
SELECT abs(value1), abs(value2), abs(value3), abs(value4) FROM s3 WHERE value2 != 200;

-- select log (builtin function, need to swap arguments, numeric cast, explain)
-- log_<base>(v) : postgresql (base, v), influxdb (v, base), mysql (base, v)
--Testcase 21:
EXPLAIN VERBOSE
SELECT log(value1::numeric, value2::numeric) FROM s3 WHERE value1 != 1;

-- select log (builtin function, need to swap arguments, numeric cast, result)
--Testcase 22:
SELECT log(value1::numeric, value2::numeric) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, float8, explain)
--Testcase 23:
EXPLAIN VERBOSE
SELECT log(value1, 0.1) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, float8, result)
--Testcase 24:
SELECT log(value1, 0.1) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, bigint, explain)
--Testcase 25:
EXPLAIN VERBOSE
SELECT log(value2, 3) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, bigint, result)
--Testcase 26:
SELECT log(value2, 3) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, mix type, explain)
--Testcase 27:
EXPLAIN VERBOSE
SELECT log(value1, value2) FROM s3 WHERE value1 != 1;

-- select log (stub function, need to swap arguments, mix type, result)
--Testcase 28:
SELECT log(value1, value2) FROM s3 WHERE value1 != 1;

-- select log2 (stub function, explain)
-- EXPLAIN VERBOSE
-- SELECT log2(value1),log2(value2) FROM s3;

-- select log2 (stub function, result)
-- SELECT log2(value1),log2(value2) FROM s3;

-- select spread (stub agg function, explain)
-- EXPLAIN VERBOSE
-- SELECT spread(value1),spread(value2),spread(value3),spread(value4) FROM s3;

-- select spread (stub agg function, result)
-- SELECT spread(value1),spread(value2),spread(value3),spread(value4) FROM s3;

-- select spread (stub agg function, raise exception if not expected type)
-- SELECT spread(value1::numeric),spread(value2::numeric),spread(value3::numeric),spread(value4::numeric) FROM s3;

-- select abs as nest function with agg (pushdown, explain)
--Testcase 29:
EXPLAIN VERBOSE
SELECT sum(value3),abs(sum(value3)) FROM s3;

-- select abs as nest function with agg (pushdown, result)
--Testcase 30:
SELECT sum(value3),abs(sum(value3)) FROM s3;

-- select abs as nest with log2 (pushdown, explain)
-- EXPLAIN VERBOSE
-- SELECT abs(log2(value1)),abs(log2(1/value1)) FROM s3;

-- select abs as nest with log2 (pushdown, result)
-- SELECT abs(log2(value1)),abs(log2(1/value1)) FROM s3;

-- select abs with non pushdown func and explicit constant (explain)
--Testcase 31:
EXPLAIN VERBOSE
SELECT abs(value3), pi(), 4.1 FROM s3;

-- select abs with non pushdown func and explicit constant (result)
--Testcase 32:
SELECT abs(value3), pi(), 4.1 FROM s3;

-- select sqrt as nest function with agg and explicit constant (pushdown, explain)
--Testcase 33:
EXPLAIN VERBOSE
SELECT sqrt(count(value1)), pi(), 4.1 FROM s3;

-- select sqrt as nest function with agg and explicit constant (pushdown, result)
--Testcase 34:
SELECT sqrt(count(value1)), pi(), 4.1 FROM s3;

-- select sqrt as nest function with agg and explicit constant and tag (error, explain)
--Testcase 35:
EXPLAIN VERBOSE
SELECT sqrt(count(value1)), pi(), 4.1, tag1 FROM s3;

-- select spread (stub agg function and group by influx_time() and tag) (explain)
-- EXPLAIN VERBOSE
-- SELECT spread("value1"),influx_time(time, interval '1s'),tag1 FROM s3 WHERE time >= to_timestamp(0) and time <= to_timestamp(4) GROUP BY influx_time(time, interval '1s'), tag1;

-- select spread (stub agg function and group by influx_time() and tag) (result)
-- SELECT spread("value1"),influx_time(time, interval '1s'),tag1 FROM s3 WHERE time >= to_timestamp(0) and time <= to_timestamp(4) GROUP BY influx_time(time, interval '1s'), tag1;

-- select spread (stub agg function and group by tag only) (result)
-- SELECT tag1,spread("value1") FROM s3 WHERE time >= to_timestamp(0) and time <= to_timestamp(4) GROUP BY tag1;

-- select spread (stub agg function and other aggs) (result)
-- SELECT sum("value1"),spread("value1"),count("value1") FROM s3;

-- select abs with order by (explain)
--Testcase 36:
EXPLAIN VERBOSE
SELECT value1, abs(1-value1) FROM s3 order by abs(1-value1);

-- select abs with order by (result)
--Testcase 37:
SELECT value1, abs(1-value1) FROM s3 order by abs(1-value1);

-- select abs with order by index (result)
--Testcase 38:
SELECT value1, abs(1-value1) FROM s3 order by 2,1;

-- select abs with order by index (result)
--Testcase 39:
SELECT value1, abs(1-value1) FROM s3 order by 1,2;

-- select abs and as
--Testcase 40:
SELECT abs(value3) as abs1 FROM s3;

-- select spread over join query (explain)
-- EXPLAIN VERBOSE
-- SELECT spread(t1.value1), spread(t2.value1) FROM s3 t1 INNER JOIN s3 t2 ON (t1.value1 = t2.value1) where t1.value1 = 0.1;

-- select spread over join query (result, stub call error)
-- SELECT spread(t1.value1), spread(t2.value1) FROM s3 t1 INNER JOIN s3 t2 ON (t1.value1 = t2.value1) where t1.value1 = 0.1;

-- select spread with having (explain)
-- EXPLAIN VERBOSE
-- SELECT spread(value1) FROM s3 HAVING spread(value1) > 100;

-- select spread with having (explain, cannot pushdown, stub call error)
-- SELECT spread(value1) FROM s3 HAVING spread(value1) > 100;

-- select abs with arithmetic and tag in the middle (explain)
--Testcase 41:
EXPLAIN VERBOSE
SELECT abs(value1) + 1, value2, tag1, sqrt(value2) FROM s3;

-- select abs with arithmetic and tag in the middle (result)
--Testcase 42:
SELECT abs(value1) + 1, value2, tag1, sqrt(value2) FROM s3;

-- select with order by limit (explain)
--Testcase 43:
EXPLAIN VERBOSE
SELECT abs(value1), abs(value3), sqrt(value2) FROM s3 ORDER BY abs(value3) LIMIT 1;

-- select with order by limit (explain)
--Testcase 44:
SELECT abs(value1), abs(value3), sqrt(value2) FROM s3 ORDER BY abs(value3) LIMIT 1;

-- select mixing with non pushdown func (all not pushdown, explain)
--Testcase 45:
EXPLAIN VERBOSE
SELECT abs(value1), sqrt(value2), chr(id+40) FROM s3;

-- select mixing with non pushdown func (result)
--Testcase 46:
SELECT abs(value1), sqrt(value2), chr(id+40) FROM s3;

--Testcase 47:
DROP FOREIGN TABLE s3;

-- full text search table
--Testcase 48:
CREATE FOREIGN TABLE ftextsearch(id int, content text) SERVER server1 OPTIONS(dbname 'mysql_fdw_regress', table_name 'ftextsearch');

-- text search (pushdown, explain)
--Testcase 49:
EXPLAIN VERBOSE
SELECT MATCH_AGAINST(ARRAY[content, 'success catches']) AS score, content FROM ftextsearch WHERE MATCH_AGAINST(ARRAY[content, 'success catches','IN BOOLEAN MODE']) != 0;

-- text search (pushdown, result)
--Testcase 50:
SELECT content FROM (
SELECT MATCH_AGAINST(ARRAY[content, 'success catches']) AS score, content FROM ftextsearch WHERE MATCH_AGAINST(ARRAY[content, 'success catches','IN BOOLEAN MODE']) != 0
       ) AS t;

--Testcase 51:
DROP FOREIGN TABLE ftextsearch;

--Testcase 52:
DROP USER MAPPING FOR CURRENT_USER SERVER server1;
--Testcase 53:
DROP SERVER server1;
--Testcase 54:
DROP EXTENSION mysql_fdw;
