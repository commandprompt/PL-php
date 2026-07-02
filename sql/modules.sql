--
-- Module autoloading (plphp_modules) and the plphp.start_proc GUC
-- (ported from PL/Tcl).
--

-- Creating any plphp function runs the validator, which loads the shared
-- library so the plphp.start_proc GUC becomes available.
CREATE FUNCTION _load() RETURNS void LANGUAGE plphp AS $$ return; $$;

-- A start proc, run once when the interpreter initializes this session.
CREATE FUNCTION my_start() RETURNS void LANGUAGE plphp AS $$
    pg_raise('notice', 'start_proc ran');
$$;
SET plphp.start_proc = 'my_start';

-- Two module rows (loaded in modseq order): one defines a function, the other
-- a class -- both must become available session-wide.
CREATE TABLE plphp_modules (modname text, modseq int, modsrc text);
INSERT INTO plphp_modules VALUES
    ('util', 0, 'function plphp_triple($n) { return $n * 3; }'),
    ('util', 1, 'class PlphpGreeter { public static function hi($who) { return "hi $who"; } }');

-- The first PL/php *call* in this session triggers module loading and the
-- start proc, then runs the body using the module-defined helper.
CREATE FUNCTION use_mod(int) RETURNS int LANGUAGE plphp AS $$
    return plphp_triple($args[0]);
$$;
SELECT use_mod(14);

-- A class defined by a module is usable too.
CREATE FUNCTION use_class(text) RETURNS text LANGUAGE plphp AS $$
    return PlphpGreeter::hi($args[0]);
$$;
SELECT use_class('bob');

DROP TABLE plphp_modules;
DROP FUNCTION use_class(text);
DROP FUNCTION use_mod(int);
DROP FUNCTION my_start();
DROP FUNCTION _load();
