--
-- Features ported from PL/Perl: DO blocks, prepared plans, quoting, elog.
--

-- Anonymous code block (DO)
DO $$
    pg_raise('notice', 'hello from a DO block');
$$ LANGUAGE plphp;

-- DO block that uses SPI
DO $$
    $r = spi_exec('select 42 as n');
    $row = spi_fetch_row($r);
    pg_raise('notice', 'DO via SPI: ' . $row['n']);
$$ LANGUAGE plphp;

-- A syntactically invalid DO block is rejected
DO $$ this is not php $$ LANGUAGE plphp;

-- Prepared statements: prepare / exec_prepared / freeplan
CREATE FUNCTION prep_test(int) RETURNS int LANGUAGE plphp AS $$
    $plan = spi_prepare('select $1 * 10 as v', 'int4');
    $r = spi_exec_prepared($plan, $args[0]);
    $row = spi_fetch_row($r);
    spi_freeplan($plan);
    return $row['v'];
$$;
SELECT prep_test(5);

-- Prepared plan iterated over multiple rows
CREATE FUNCTION prep_sum(int) RETURNS int LANGUAGE plphp AS $$
    $plan = spi_prepare('select g from generate_series(1, $1) g', 'int4');
    $r = spi_exec_prepared($plan, $args[0]);
    $total = 0;
    while ($row = spi_fetch_row($r))
        $total += $row['g'];
    spi_freeplan($plan);
    return intval($total);
$$;
SELECT prep_sum(5);

-- Prepared plan with two text arguments, and spi_processed
CREATE FUNCTION prep_concat(text, text) RETURNS text LANGUAGE plphp AS $$
    $plan = spi_prepare('select $1 || $2 as s', 'text', 'text');
    $r = spi_exec_prepared($plan, $args[0], $args[1]);
    pg_raise('notice', 'rows: ' . spi_processed($r));
    $row = spi_fetch_row($r);
    spi_freeplan($plan);
    return $row['s'];
$$;
SELECT prep_concat('foo', 'bar');

-- spi_query_prepared is an alias of spi_exec_prepared
CREATE FUNCTION prep_alias(int) RETURNS int LANGUAGE plphp AS $$
    $plan = spi_prepare('select $1 + 1 as v', 'int4');
    $r = spi_query_prepared($plan, $args[0]);
    $row = spi_fetch_row($r);
    spi_freeplan($plan);
    return $row['v'];
$$;
SELECT prep_alias(41);

-- Quoting helpers
CREATE FUNCTION quoting() RETURNS text LANGUAGE plphp AS $$
    $lit = quote_literal("O'Reilly");
    $id  = quote_ident('weird name');
    $nul = quote_nullable(null);
    $val = quote_nullable('x');
    return "$lit | $id | $nul | $val";
$$;
SELECT quoting();

-- elog with the various levels
CREATE FUNCTION elog_levels() RETURNS void LANGUAGE plphp AS $$
    elog('INFO', 'info via elog');
    elog('NOTICE', 'notice via elog');
    elog('WARNING', 'warning via elog');
$$;
SELECT elog_levels();

-- elog ERROR aborts the statement
CREATE FUNCTION elog_error() RETURNS void LANGUAGE plphp AS $$
    elog('ERROR', 'boom from elog');
$$;
SELECT elog_error();

-- A prepared statement with a NULL argument
CREATE FUNCTION prep_null() RETURNS text LANGUAGE plphp AS $$
    $plan = spi_prepare('select $1::text as v', 'text');
    $r = spi_exec_prepared($plan, null);
    $row = spi_fetch_row($r);
    spi_freeplan($plan);
    return $row['v'] === null ? 'null ok' : 'got: ' . $row['v'];
$$;
SELECT prep_null();

-- A prepared statement with no placeholders
CREATE FUNCTION prep_noargs() RETURNS int LANGUAGE plphp AS $$
    $plan = spi_prepare('select 7 as v');
    $r = spi_exec_prepared($plan);
    $row = spi_fetch_row($r);
    spi_freeplan($plan);
    return intval($row['v']);
$$;
SELECT prep_noargs();

-- quote_ident only quotes identifiers that need it
CREATE FUNCTION quoting2() RETURNS text LANGUAGE plphp AS $$
    return quote_ident('simple') . ' | ' . quote_ident('Mixed Case');
$$;
SELECT quoting2();

-- elog with an unrecognized level is an error
CREATE FUNCTION elog_bad() RETURNS void LANGUAGE plphp AS $$
    elog('BOGUS', 'nope');
$$;
SELECT elog_bad();
