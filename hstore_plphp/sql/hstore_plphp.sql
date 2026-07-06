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

DROP EXTENSION hstore_plphp CASCADE;
