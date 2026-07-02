--
-- Explicit subtransactions (ported from PL/Tcl): subtransaction(callable, ...).
--

CREATE TABLE sx (id int);

-- A successful subtransaction commits its work.
CREATE FUNCTION sx_ok() RETURNS int LANGUAGE plphp AS $$
    subtransaction(function() { spi_exec("insert into sx values (1)"); });
    return 1;
$$;
SELECT sx_ok();
SELECT count(*) AS after_ok FROM sx;

-- A PHP exception thrown in the body rolls the subtransaction back and is
-- catchable by the caller.
CREATE FUNCTION sx_catch() RETURNS text LANGUAGE plphp AS $$
    try {
        subtransaction(function() {
            spi_exec("insert into sx values (2)");
            throw new Exception("rollback me");
        });
    } catch (\Throwable $e) {
        return "caught: " . $e->getMessage();
    }
    return "no exception";
$$;
SELECT sx_catch();
-- The insert of 2 was rolled back, so the count is unchanged.
SELECT count(*) AS after_catch FROM sx;

-- Arguments after the callable are passed through to it, and the callable's
-- return value is returned.
CREATE FUNCTION sx_ret(int) RETURNS int LANGUAGE plphp AS $$
    return subtransaction(function($x) { return $x * 2; }, $args[0]);
$$;
SELECT sx_ret(21);

-- Nested subtransactions: the inner one rolls back (its exception is caught)
-- while the outer one commits.
CREATE FUNCTION sx_nested() RETURNS void LANGUAGE plphp AS $$
    subtransaction(function() {
        spi_exec("insert into sx values (10)");
        try {
            subtransaction(function() {
                spi_exec("insert into sx values (11)");
                throw new Exception("inner fails");
            });
        } catch (\Throwable $e) {
            pg_raise('notice', 'inner rolled back: ' . $e->getMessage());
        }
    });
$$;
SELECT sx_nested();
-- 1 was already present; 10 committed, 11 rolled back.
SELECT id FROM sx ORDER BY id;

-- A database error inside a subtransaction rolls it back and aborts the
-- statement -- it is not catchable as a PHP exception.
CREATE FUNCTION sx_dberr() RETURNS void LANGUAGE plphp AS $$
    try {
        subtransaction(function() { spi_exec("insert into sx values (1/0)"); });
    } catch (\Throwable $e) {
        pg_raise('notice', 'this is not reached for a database error');
    }
$$;
SELECT sx_dberr();
SELECT count(*) AS after_dberr FROM sx;

DROP TABLE sx;
