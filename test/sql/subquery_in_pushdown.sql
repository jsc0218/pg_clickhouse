\unset ECHO
SET client_min_messages = notice;
SET session timezone = 'UTC';

-- The hook is a planner hook, so the library has to be loaded first.
LOAD 'pg_clickhouse';

-- x IN (SELECT ... GROUP BY ... HAVING ...) over ClickHouse tables used to
-- become a semi-join whose inner side cannot be pushed down, so the foreign
-- tables were read in full. It is now kept as a SubPlan and shipped.

CREATE SERVER siq_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'siq_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER siq_svr;

CREATE SERVER siq_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER siq_admin;

CALL clickhouse_perform('siq_admin', 'DROP DATABASE IF EXISTS siq_test');
CALL clickhouse_perform('siq_admin', 'CREATE DATABASE siq_test');
CALL clickhouse_perform('siq_admin', $$
    CREATE TABLE siq_test.orders (
        o_orderkey UInt32 NOT NULL,
        o_custkey  UInt32 NOT NULL,
        o_total    Decimal(10, 2) NOT NULL
    ) ENGINE = MergeTree ORDER BY o_orderkey;
$$);
CALL clickhouse_perform('siq_admin', $$
    CREATE TABLE siq_test.lines (
        l_orderkey UInt32 NOT NULL,
        l_quantity Decimal(10, 2) NOT NULL
    ) ENGINE = MergeTree ORDER BY l_orderkey;
$$);
CALL clickhouse_perform('siq_admin', $$
    INSERT INTO siq_test.orders
    SELECT number + 1, (number % 4) + 1, (number + 1) * 10 FROM numbers(12);
$$);
-- 3 lines per order; orders 1..12 get quantity 4 * orderkey in total
CALL clickhouse_perform('siq_admin', $$
    INSERT INTO siq_test.lines
    SELECT (number % 12) + 1, ((number % 12) + 1) * 4 / 3 FROM numbers(36);
$$);

CREATE SCHEMA siq;
IMPORT FOREIGN SCHEMA siq_test FROM SERVER siq_svr INTO siq;

\echo -- IN (aggregate subquery) is pushed down as part of the remote query
EXPLAIN (COSTS OFF)
SELECT o.o_orderkey, sum(l.l_quantity)
FROM siq.orders o, siq.lines l
WHERE o.o_orderkey IN (SELECT l_orderkey FROM siq.lines
                       GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
  AND o.o_orderkey = l.l_orderkey
GROUP BY o.o_orderkey ORDER BY o.o_orderkey;

SELECT o.o_orderkey, sum(l.l_quantity)
FROM siq.orders o, siq.lines l
WHERE o.o_orderkey IN (SELECT l_orderkey FROM siq.lines
                       GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
  AND o.o_orderkey = l.l_orderkey
GROUP BY o.o_orderkey ORDER BY o.o_orderkey;

\echo -- with the setting off the planner keeps its semi-join, same rows
-- Count the foreign scans rather than print the plan: the join method the
-- planner picks for the local part differs between releases.
CREATE FUNCTION siq_foreign_scans(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
    line text;
    n int := 0;
BEGIN
    FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF line LIKE '%Foreign Scan%' THEN
            n := n + 1;
        END IF;
    END LOOP;
    RETURN n;
END
$$;
SET pg_clickhouse.pushdown_subquery_in = off;
SELECT siq_foreign_scans($q$
SELECT o.o_orderkey, sum(l.l_quantity)
FROM siq.orders o, siq.lines l
WHERE o.o_orderkey IN (SELECT l_orderkey FROM siq.lines
                       GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
  AND o.o_orderkey = l.l_orderkey
GROUP BY o.o_orderkey ORDER BY o.o_orderkey
$q$) > 1 AS not_a_single_remote_query;

SELECT o.o_orderkey, sum(l.l_quantity)
FROM siq.orders o, siq.lines l
WHERE o.o_orderkey IN (SELECT l_orderkey FROM siq.lines
                       GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
  AND o.o_orderkey = l.l_orderkey
GROUP BY o.o_orderkey ORDER BY o.o_orderkey;
RESET pg_clickhouse.pushdown_subquery_in;

\echo -- the sublink inside a view is kept pushable too
CREATE TEMP VIEW siq_big_orders AS
SELECT o.o_orderkey, sum(l.l_quantity) AS qty
FROM siq.orders o, siq.lines l
WHERE o.o_orderkey IN (SELECT l_orderkey FROM siq.lines
                       GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
  AND o.o_orderkey = l.l_orderkey
GROUP BY o.o_orderkey;
SELECT siq_foreign_scans($q$SELECT * FROM siq_big_orders$q$) = 1
    AS single_remote_query;
SELECT * FROM siq_big_orders ORDER BY o_orderkey;
DROP VIEW siq_big_orders;

\echo -- a local table in the tested expression is left alone
CREATE TEMP TABLE siq_local (k int);
INSERT INTO siq_local VALUES (9), (12);
SELECT k FROM siq_local
WHERE k IN (SELECT l_orderkey FROM siq.lines
            GROUP BY l_orderkey HAVING sum(l_quantity) > 30)
ORDER BY k;

-- Clean up.
DROP FUNCTION siq_foreign_scans(text);
DROP SCHEMA siq CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER siq_svr;
DROP SERVER siq_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER siq_admin;
DROP SERVER siq_admin CASCADE;
