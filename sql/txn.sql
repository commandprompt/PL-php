--
-- Transaction control in procedures: spi_commit / spi_rollback.
-- (Ported from PL/Perl; only valid in a procedure invoked by CALL in a
-- non-atomic context.)
--

CREATE TABLE txn_test (id int);

-- A procedure that commits after each insert.
CREATE PROCEDURE do_commits() LANGUAGE plphp AS $$
    for ($i = 1; $i <= 3; $i++) {
        spi_exec("insert into txn_test values ($i)");
        spi_commit();
    }
$$;

CALL do_commits();
SELECT count(*) AS committed FROM txn_test;

-- A procedure that rolls back its work.
CREATE PROCEDURE do_rollback() LANGUAGE plphp AS $$
    spi_exec("insert into txn_test values (99)");
    spi_rollback();
$$;

CALL do_rollback();
-- The rolled-back insert must not be visible; still 3 rows.
SELECT count(*) AS after_rollback FROM txn_test;

-- Committing is not allowed in an ordinary function (atomic context).
CREATE FUNCTION cannot_commit() RETURNS void LANGUAGE plphp AS $$
    spi_commit();
$$;
SELECT cannot_commit();

-- Nor in a procedure called inside an explicit transaction block.
BEGIN;
CALL do_commits();
ROLLBACK;

DROP TABLE txn_test;
