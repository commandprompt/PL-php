CREATE EXTENSION jsonb_plphp CASCADE;

-- round trips: scalars, arrays, objects, nesting
CREATE FUNCTION roundtrip(val jsonb) RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	return $args[0];
$$;
-- like jsonb_plperl: a JSON null becomes PHP null, and returning PHP null
-- yields SQL NULL (not jsonb 'null')
SELECT roundtrip('null');
SELECT roundtrip('true');
SELECT roundtrip('1');
SELECT roundtrip('2.5');
SELECT roundtrip('"some text"');
SELECT roundtrip('[]');
-- an empty PHP array is indistinguishable from an empty list, so {} comes
-- back as [] (the same ambiguity PHP's json_encode has)
SELECT roundtrip('{}');
SELECT roundtrip('[1, [2, "x", null], {"a": true}]');
SELECT roundtrip('{"n": 1e2, "b": false, "s": "x", "nested": {"list": [1, 2]}}');
SELECT roundtrip('9007199254740993');

-- what PHP actually sees: native types, not text
CREATE FUNCTION typeinfo(val jsonb) RETURNS text
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	$t = function ($v) use (&$t) {
		if (is_array($v)) {
			$parts = array();
			foreach ($v as $k => $e)
				$parts[] = "$k:" . $t($e);
			return "{" . implode(",", $parts) . "}";
		}
		if ($v === null)  return "null";
		if (is_bool($v))  return "bool";
		if (is_int($v))   return "int";
		if (is_float($v)) return "float";
		return "string";
	};
	return $t($args[0]);
$$;
SELECT typeinfo('{"i": 5, "f": 1.5, "b": true, "n": null, "s": "x", "l": [1, 2]}');

-- build jsonb from PHP data structures
CREATE FUNCTION makedoc() RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	return array(
		"id"      => 7,
		"tags"    => array("a", "b"),
		"ok"      => true,
		"score"   => 9.5,
		"nothing" => null
	);
$$;
SELECT makedoc();

-- scalar return
CREATE FUNCTION makenum() RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	return 42;
$$;
SELECT makenum();

-- a real transformation: redact keys recursively on native arrays
CREATE FUNCTION redact(doc jsonb, key text) RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	$walk = function (&$node) use (&$walk, $args) {
		foreach ($node as $k => &$v) {
			if ((string) $k === $args[1])
				$v = "[redacted]";
			elseif (is_array($v))
				$walk($v);
		}
	};
	$walk($args[0]);
	return $args[0];
$$;
SELECT redact('{"user": "jd", "auth": {"password": "x"}, "password": "y"}', 'password');

-- without the TRANSFORM clause, jsonb still arrives as its text form
CREATE FUNCTION astext(val jsonb) RETURNS text LANGUAGE plphp AS $$
	return gettype($args[0]) . ": " . $args[0];
$$;
SELECT astext('{"a": 1}');

-- a PHP object has no jsonb representation
CREATE FUNCTION badret() RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	return new stdClass();
$$;
SELECT badret();

-- The transform reaches nested contexts (as for hstore): a trigger sees a
-- jsonb column as a PHP value and can 'MODIFY' it, and a SETOF jsonb function
-- emits native values row by row.
CREATE TABLE jb_events (id int, payload jsonb);
CREATE FUNCTION jb_tag() RETURNS trigger
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	$p = $_TD['new']['payload'];
	$p['seen'] = true;
	$p['n'] = ($p['n'] ?? 0) + 1;
	$_TD['new']['payload'] = $p;
	return 'MODIFY';
$$;
CREATE TRIGGER jb_tag_t BEFORE INSERT ON jb_events
	FOR EACH ROW EXECUTE PROCEDURE jb_tag();
INSERT INTO jb_events VALUES (1, '{"n": 4}');
SELECT id, payload FROM jb_events;
DROP TABLE jb_events;

CREATE FUNCTION jb_series(int) RETURNS SETOF jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
	for ($i = 1; $i <= $args[0]; $i++)
		return_next(array('i' => $i, 'sq' => $i * $i));
	return;
$$;
SELECT * FROM jb_series(3);

DROP FUNCTION roundtrip(jsonb), typeinfo(jsonb), makedoc(), makenum(),
              redact(jsonb, text), astext(jsonb), badret(),
              jb_tag(), jb_series(int);
