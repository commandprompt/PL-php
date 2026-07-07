CREATE EXTENSION bytea_plphp CASCADE;

-- A bytea argument arrives as a raw PHP string; a returned string becomes
-- bytea verbatim. Round-trip an ordinary value.
CREATE FUNCTION roundtrip(val bytea) RETURNS bytea
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	return $args[0];
$$;

SELECT roundtrip('\x00ff01'::bytea);
SELECT roundtrip('hello'::bytea);
-- empty bytea
SELECT roundtrip(''::bytea);

-- Binary-safe: embedded NUL bytes survive both directions (the default text
-- path would truncate them). strlen sees the true byte count.
CREATE FUNCTION bytelen(val bytea) RETURNS int
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	return strlen($args[0]);
$$;

SELECT bytelen('\x00010200'::bytea);
SELECT length(roundtrip('\x00010200'::bytea));

-- What PHP sees really is bytes, not the "\x..." text form: index into them.
CREATE FUNCTION byteat(val bytea, idx int) RETURNS int
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	return ord($args[0][$args[1]]);
$$;

SELECT byteat('\xdeadbeef'::bytea, 0);
SELECT byteat('\xdeadbeef'::bytea, 3);

-- Build a bytea from raw bytes in PHP.
CREATE FUNCTION make_bytes() RETURNS bytea
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	return chr(0) . chr(255) . chr(16);
$$;

SELECT make_bytes();

-- NULL passes through as SQL NULL (the transform is STRICT / null-safe).
SELECT roundtrip(NULL::bytea) IS NULL AS is_null;

-- Nested context: a bytea column inside a composite result also gets the
-- transform (the #30 nested-transform machinery), so PHP works with raw bytes
-- when building the row.
CREATE TYPE blob_row AS (tag text, data bytea);

CREATE FUNCTION make_row(t text, d bytea) RETURNS blob_row
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	// $args[1] is raw bytes; prepend a byte and return the row
	return array('tag' => $args[0], 'data' => chr(1) . $args[1]);
$$;

SELECT (make_row('x', '\xaabb'::bytea)).*;

-- Returning a non-string PHP value for bytea is an error.
CREATE FUNCTION bad_return() RETURNS bytea
LANGUAGE plphp TRANSFORM FOR TYPE bytea AS $$
	return array(1, 2, 3);
$$;

SELECT bad_return();

DROP TYPE blob_row CASCADE;
