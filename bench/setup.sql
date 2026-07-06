-- benchmark functions: identical logic in plphp / plpgsql / plperl
CREATE EXTENSION IF NOT EXISTS plphp;
CREATE EXTENSION IF NOT EXISTS plperl;

-- 1. scalar math
CREATE OR REPLACE FUNCTION math_php(a int, b int) RETURNS int LANGUAGE plphp AS $$
    return ($args[0] * 3 + $args[1]) % 97;
$$;
CREATE OR REPLACE FUNCTION math_pgsql(a int, b int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN RETURN (a * 3 + b) % 97; END;
$$;
CREATE OR REPLACE FUNCTION math_perl(a int, b int) RETURNS int LANGUAGE plperl AS $$
    return ($_[0] * 3 + $_[1]) % 97;
$$;

-- 2. string work
CREATE OR REPLACE FUNCTION str_php(t text) RETURNS text LANGUAGE plphp AS $$
    return strtoupper(strrev($args[0])) . strlen($args[0]);
$$;
CREATE OR REPLACE FUNCTION str_pgsql(t text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN RETURN upper(reverse(t)) || length(t); END;
$$;
CREATE OR REPLACE FUNCTION str_perl(t text) RETURNS text LANGUAGE plperl AS $$
    return uc(reverse($_[0])) . length($_[0]);
$$;

-- 3. SPI row loop over a 1000-row table
DROP TABLE IF EXISTS bench_rows;
CREATE TABLE bench_rows AS SELECT g AS id, g * 2 AS val FROM generate_series(1, 1000) g;
CREATE OR REPLACE FUNCTION rows_php() RETURNS bigint LANGUAGE plphp AS $$
    $r = spi_exec("select val from bench_rows");
    $s = 0;
    while ($row = spi_fetch_row($r))
        $s += $row['val'];
    return $s;
$$;
CREATE OR REPLACE FUNCTION rows_pgsql() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE s bigint := 0; r record;
BEGIN
    FOR r IN SELECT val FROM bench_rows LOOP s := s + r.val; END LOOP;
    RETURN s;
END;
$$;
CREATE OR REPLACE FUNCTION rows_perl() RETURNS bigint LANGUAGE plperl AS $$
    my $rv = spi_exec_query("select val from bench_rows");
    my $s = 0;
    $s += $_->{val} for @{$rv->{rows}};
    return $s;
$$;

-- 4. repeated small SPI statements (10 queries per call)
CREATE OR REPLACE FUNCTION spi_php(n int) RETURNS int LANGUAGE plphp AS $$
    $s = 0;
    for ($i = 0; $i < 10; $i++) {
        $r = spi_exec("select " . ($args[0] + $i) . " as x");
        $row = spi_fetch_row($r);
        $s += $row['x'];
    }
    return $s;
$$;
CREATE OR REPLACE FUNCTION spi_pgsql(n int) RETURNS int LANGUAGE plpgsql AS $$
DECLARE s int := 0; x int;
BEGIN
    FOR i IN 0..9 LOOP
        EXECUTE 'select ' || (n + i) INTO x;
        s := s + x;
    END LOOP;
    RETURN s;
END;
$$;
CREATE OR REPLACE FUNCTION spi_perl(n int) RETURNS int LANGUAGE plperl AS $$
    my $s = 0;
    for my $i (0..9) {
        my $rv = spi_exec_query("select " . ($_[0] + $i) . " as x");
        $s += $rv->{rows}[0]{x};
    }
    return $s;
$$;
