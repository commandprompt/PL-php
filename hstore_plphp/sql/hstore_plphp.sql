--
-- hstore <-> PHP array transform.  Functions must opt in with
-- TRANSFORM FOR TYPE hstore.
--
CREATE EXTENSION hstore_plphp CASCADE;

-- An hstore argument arrives as an associative array of string => string/null.
CREATE FUNCTION ht_classes(hstore) RETURNS text
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    $h = $args[0];
    ksort($h);
    $out = [];
    foreach ($h as $k => $v)
        $out[] = "$k=" . (is_null($v) ? 'NULL' : gettype($v) . "($v)");
    return implode(' ', $out);
$$;
SELECT ht_classes('a=>1, b=>NULL, "key with spaces"=>"and \"quotes\""'::hstore);

-- Round-trip: modify and return; null becomes an hstore NULL.
CREATE FUNCTION ht_upcase(hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    $out = [];
    foreach ($args[0] as $k => $v)
        $out[strtoupper($k)] = is_null($v) ? null : strtoupper($v);
    return $out;
$$;
SELECT ht_upcase('name=>widget, note=>NULL');

-- Build an hstore from PHP data: keys and scalar values are stringified,
-- and a PHP null becomes an hstore NULL.
CREATE FUNCTION ht_build(int) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    return ['n' => $args[0], 'double' => $args[0] * 2,
            'ok' => $args[0] % 2 == 1, 'gone' => null];
$$;
SELECT ht_build(3);
SELECT ht_build(4)->'double' AS doubled;

-- Idiomatic array work: merge and prune the nulls in one step.
CREATE FUNCTION ht_merge(a hstore, b hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    $m = array_merge($args[0], $args[1]);
    return array_filter($m, fn($v) => !is_null($v));
$$;
SELECT ht_merge('a=>1, b=>2', 'b=>20, c=>NULL, d=>4');

-- The empty hstore round-trips.
CREATE FUNCTION ht_empty(hstore) RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    return $args[0];
$$;
SELECT ht_empty(''::hstore);

-- A NULL hstore argument is null; returning null is SQL NULL.
SELECT ht_empty(NULL) IS NULL AS null_roundtrip;

-- Returning something other than an array is rejected cleanly.
CREATE FUNCTION ht_bad() RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    return "not an array";
$$;
SELECT ht_bad();

-- A nested value (array) is not a valid hstore value.
CREATE FUNCTION ht_nested() RETURNS hstore
TRANSFORM FOR TYPE hstore
LANGUAGE plphp AS $$
    return ['k' => ['nested']];
$$;
SELECT ht_nested();

-- Without the TRANSFORM clause, hstore still arrives as its text form.
CREATE FUNCTION ht_untransformed(hstore) RETURNS text LANGUAGE plphp AS $$
    return gettype($args[0]);
$$;
SELECT ht_untransformed('a=>1'::hstore);

--
-- The transform reaches nested contexts, in both directions: trigger rows,
-- composite arguments and results, and set-returning results.
--

-- Trigger: $_TD['new']['<hstore col>'] is a PHP array, and 'MODIFY' takes one
-- back.
CREATE TABLE ht_items (id int, attrs hstore);
CREATE FUNCTION ht_normalize() RETURNS trigger
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    $a = $_TD['new']['attrs'];
    elog('NOTICE', 'attrs is a ' . gettype($a));
    $out = array('checked' => 'yes');
    foreach ($a as $k => $v) $out[strtolower($k)] = $v;
    $_TD['new']['attrs'] = $out;
    return 'MODIFY';
$$;
CREATE TRIGGER ht_items_norm BEFORE INSERT ON ht_items
    FOR EACH ROW EXECUTE PROCEDURE ht_normalize();
INSERT INTO ht_items VALUES (1, 'Color=>red, SIZE=>xl');
SELECT id, attrs FROM ht_items;
DROP TABLE ht_items;

-- Composite argument with an hstore field arrives as a PHP array.
CREATE TYPE ht_rec AS (id int, meta hstore);
CREATE FUNCTION ht_field(ht_rec) RETURNS text
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    $m = $args[0]['meta'];
    return is_array($m) ? ($m['k'] ?? '?') : "not-array";
$$;
SELECT ht_field(ROW(1, 'k=>v')::ht_rec);

-- Composite result with an hstore field is built from a PHP array.
CREATE FUNCTION ht_make(int) RETURNS ht_rec
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    return array('id' => $args[0], 'meta' => array('n' => $args[0], 'ok' => 'y'));
$$;
SELECT * FROM ht_make(7);

-- SETOF hstore via return_next: each row is a PHP array.
CREATE FUNCTION ht_shatter(hstore) RETURNS SETOF hstore
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    $h = $args[0];
    ksort($h);
    foreach ($h as $k => $v) return_next(array($k => $v));
    return;
$$;
SELECT * FROM ht_shatter('b=>2, a=>1');

-- RETURNS TABLE with an hstore column.
CREATE FUNCTION ht_table(int) RETURNS TABLE(n int, tags hstore)
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    for ($i = 1; $i <= $args[0]; $i++) { $n = $i; $tags = array('sq' => $i * $i); return_next(); }
    return;
$$;
SELECT * FROM ht_table(3);

-- Rows read back from SPI are NOT transformed (they cross as text), matching
-- the top-level "no TRANSFORM" behavior.
CREATE FUNCTION ht_via_spi() RETURNS text
TRANSFORM FOR TYPE hstore LANGUAGE plphp AS $$
    $r = spi_exec("select 'x=>1'::hstore as h");
    $row = spi_fetch_row($r);
    return gettype($row['h']);
$$;
SELECT ht_via_spi();

-- Tear down. The CASCADE notice lists dependent objects in a
-- version-dependent way (how PostgreSQL records TRANSFORM dependencies has
-- changed across releases), so silence it to keep the test output stable.
SET client_min_messages = warning;
DROP EXTENSION hstore_plphp CASCADE;
RESET client_min_messages;
