--
-- Event trigger functions (ported from PL/Tcl).
--

CREATE FUNCTION snitch() RETURNS event_trigger LANGUAGE plphp AS $$
    pg_raise('notice', "event: {$_TD['event']} tag: {$_TD['tag']}");
$$;

CREATE EVENT TRIGGER snitch_start ON ddl_command_start EXECUTE FUNCTION snitch();

CREATE TABLE et_test (x int);
ALTER TABLE et_test ADD COLUMN y int;
DROP TABLE et_test;

DROP EVENT TRIGGER snitch_start;
DROP FUNCTION snitch();
