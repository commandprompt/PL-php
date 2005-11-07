--
-- Test SRF
--

CREATE TYPE phprow AS (f1 integer, f2 text, f3 text);

CREATE FUNCTION php_set (integer, text)
RETURNS SETOF phprow LANGUAGE plphp
AS $$
$ret[0][f1] = $args[0];
$ret[0][f2] = "hell" . $args[1];
$ret[0][f3] = "world";

$ret[1][f1] = 2 * $args[0];
$ret[1][f2] = "hell" . $args[1];
$ret[1][f3] = "postgres";

$ret[2][f1] = 3 * $args[0];
$ret[2][f2] = "hell" . $args[1];
$ret[2][f3] = "plphp";
return $ret;
$$;

SELECT * FROM php_set(999, 'o');


