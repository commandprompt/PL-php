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

-- ddl_command_end carries a different event value.
CREATE FUNCTION snitch_end() RETURNS event_trigger LANGUAGE plphp AS $$
    pg_raise('notice', "end: {$_TD['event']} tag: {$_TD['tag']}");
$$;
CREATE EVENT TRIGGER snitch_e ON ddl_command_end EXECUTE FUNCTION snitch_end();
CREATE TABLE et2 (a int);
DROP TABLE et2;
DROP EVENT TRIGGER snitch_e;
DROP FUNCTION snitch_end();

-- An event trigger can veto a command by raising an error.
CREATE FUNCTION no_create() RETURNS event_trigger LANGUAGE plphp AS $$
    pg_raise('error', 'no new tables allowed');
$$;
CREATE EVENT TRIGGER guard ON ddl_command_start WHEN TAG IN ('CREATE TABLE')
    EXECUTE FUNCTION no_create();
CREATE TABLE blocked (x int);
DROP EVENT TRIGGER guard;
DROP FUNCTION no_create();
