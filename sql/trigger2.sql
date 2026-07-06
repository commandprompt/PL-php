--
-- Trigger coverage beyond BEFORE/AFTER INSERT/UPDATE/DELETE rows:
-- statement-level TRUNCATE triggers, INSTEAD OF view triggers, and WHEN(...).
--

-- A statement-level TRUNCATE trigger fires with $_TD['event'] = 'TRUNCATE'.
CREATE TABLE trunc_t (a int);
INSERT INTO trunc_t VALUES (1), (2), (3);
CREATE FUNCTION trunc_trig() RETURNS trigger LANGUAGE plphp AS $$
    elog('NOTICE', "truncate trigger: when={$_TD['when']} event={$_TD['event']} level={$_TD['level']}");
    return null;
$$;
CREATE TRIGGER trunc_before BEFORE TRUNCATE ON trunc_t
    FOR EACH STATEMENT EXECUTE PROCEDURE trunc_trig();
CREATE TRIGGER trunc_after AFTER TRUNCATE ON trunc_t
    FOR EACH STATEMENT EXECUTE PROCEDURE trunc_trig();
TRUNCATE trunc_t;
SELECT count(*) AS rows_after_truncate FROM trunc_t;
DROP TABLE trunc_t;

-- INSTEAD OF triggers on a view: the trigger does the real work against the
-- base table and returns (non-NULL) to mark the row handled.  'when' is
-- 'INSTEAD OF'.
CREATE TABLE base_t (id int PRIMARY KEY, val text);
CREATE VIEW base_v AS SELECT id, val FROM base_t;
CREATE FUNCTION base_v_ins() RETURNS trigger LANGUAGE plphp AS $$
    elog('NOTICE', "INSTEAD OF {$_TD['event']} (when={$_TD['when']}, level={$_TD['level']})");
    spi_exec("insert into base_t values (" . intval($_TD['new']['id']) . ", " .
             quote_literal(strtoupper($_TD['new']['val'])) . ")");
    return null;   /* row handled */
$$;
CREATE FUNCTION base_v_del() RETURNS trigger LANGUAGE plphp AS $$
    spi_exec("delete from base_t where id = " . intval($_TD['old']['id']));
    return null;
$$;
CREATE TRIGGER base_v_ins_t INSTEAD OF INSERT ON base_v
    FOR EACH ROW EXECUTE PROCEDURE base_v_ins();
CREATE TRIGGER base_v_del_t INSTEAD OF DELETE ON base_v
    FOR EACH ROW EXECUTE PROCEDURE base_v_del();
INSERT INTO base_v VALUES (1, 'hello'), (2, 'world');
SELECT * FROM base_t ORDER BY id;
DELETE FROM base_v WHERE id = 1;
SELECT * FROM base_t ORDER BY id;
DROP VIEW base_v;
DROP TABLE base_t;

-- WHEN(...) gates whether the trigger fires at all (evaluated by PostgreSQL,
-- so the function only runs for matching rows).
CREATE TABLE when_t (a int, b text);
CREATE FUNCTION when_trig() RETURNS trigger LANGUAGE plphp AS $$
    elog('NOTICE', "fired for a={$_TD['new']['a']}");
    return null;
$$;
CREATE TRIGGER when_big BEFORE INSERT ON when_t
    FOR EACH ROW WHEN (NEW.a > 10) EXECUTE PROCEDURE when_trig();
INSERT INTO when_t VALUES (5, 'small'), (20, 'big'), (30, 'bigger');
SELECT count(*) AS inserted FROM when_t;
DROP TABLE when_t;

DROP FUNCTION trunc_trig(), base_v_ins(), base_v_del(), when_trig();
