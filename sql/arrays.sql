-- array conversion hardening: NULL elements, escaping, unquoted text,
-- numeric element types, empty arrays, bytea round-trips

-- returning an array containing a PHP null produces a SQL NULL element
create function arr_with_null() returns int[] language plphp as $$
	return array(1, null, 3);
$$;
select arr_with_null();

-- text elements needing quoting/escaping survive the round trip out...
create function arr_specials_out() returns text[] language plphp as $$
	return array('plain', 'has"quote', 'back\\slash', 'com,ma', '{brace}', 'sp ace', '');
$$;
select arr_specials_out();

-- ... and back in again (returns each element base64'd so the diff is unambiguous)
create function arr_specials_in(text[]) returns text language plphp as $$
	$out = array();
	foreach ($args[0] as $el)
		$out[] = $el === null ? "NULL" : base64_encode($el);
	return implode(" ", $out);
$$;
select arr_specials_in(array['plain', 'has"quote', 'back\slash', 'com,ma', '{brace}', 'sp ace', '', null]);

-- unquoted text elements (PG quotes only when needed) arrive as strings
create function arr_unquoted(text[]) returns text language plphp as $$
	return implode("|", $args[0]) . " (" . gettype($args[0][0]) . ")";
$$;
select arr_unquoted(array['foo', 'bar', 'baz']);

-- numeric arrays keep PHP numeric types
create function arr_types(int[], float8[]) returns text language plphp as $$
	return gettype($args[0][0]) . " " . gettype($args[1][0]);
$$;
select arr_types(array[1, 2], array[1.5, 2.5]);

-- int array containing a NULL on input
create function arr_null_in(int[]) returns int language plphp as $$
	$sum = 0; $nulls = 0;
	foreach ($args[0] as $el)
	{
		if ($el === null) $nulls++;
		else $sum += $el;
	}
	return $sum * 10 + $nulls;
$$;
select arr_null_in(array[1, null, 3]);

-- empty arrays, both directions
create function arr_empty_out() returns int[] language plphp as $$
	return array();
$$;
select arr_empty_out();
create function arr_empty_in(int[]) returns int language plphp as $$
	return count($args[0]);
$$;
select arr_empty_in('{}');

-- multi-dimensional array of strings needing quoting
create function arr_ndim_text() returns text[] language plphp as $$
	return array(array('a"b', 'c,d'), array('{e}', 'f\\g'));
$$;
select arr_ndim_text();

-- multi-dimensional input indexes correctly
create function arr_ndim_in(text[]) returns text language plphp as $$
	return $args[0][1][0];
$$;
select arr_ndim_in(array[array['a', 'b'], array['see', 'd']]);

-- bytea round-trip: PL/php sees the \x hex output form
create function bytea_roundtrip(bytea) returns bytea language plphp as $$
	return $args[0];
$$;
select bytea_roundtrip('\x0001ff68656c6c6f');
create function bytea_make(text) returns bytea language plphp as $$
	return "\\x" . bin2hex($args[0]);
$$;
select bytea_make('hi there');
create function bytea_read(bytea) returns text language plphp as $$
	return strtoupper(substr($args[0], 2));
$$;
select bytea_read('\xdeadbeef');

-- array-typed columns inside rows arrive as PHP arrays (not "{...}" strings):
-- via SPI rows...
create table arrt (id int, tags text[], nums int[]);
insert into arrt values (1, array['red', 'b"lue'], array[10, 20, 30]);
create function arr_in_row() returns text language plphp as $$
	$r = spi_exec("select * from arrt");
	$row = spi_fetch_row($r);
	return gettype($row['tags']) . " " . $row['tags'][1] . " " . array_sum($row['nums']);
$$;
select arr_in_row();

-- ...via cursor rows...
create function arr_in_cursor() returns int language plphp as $$
	$c = spi_query("select nums from arrt");
	$row = spi_fetchrow($c);
	return count($row['nums']);
$$;
select arr_in_cursor();

-- ...in $_TD for triggers, and writable back through MODIFY
create function arr_trig() returns trigger language plphp as $$
	pg_raise('notice', 'tags is ' . gettype($_TD['new']['tags'])
		. ' with ' . count($_TD['new']['tags']) . ' elements');
	$_TD['new']['tags'][] = 'added';
	return 'MODIFY';
$$;
create trigger arrt_trg before insert on arrt
	for each row execute procedure arr_trig();
insert into arrt values (2, array['green'], array[1]);
select tags from arrt where id = 2;
drop trigger arrt_trg on arrt;

-- ...and in composite-type arguments
create type with_arr as (label text, vals int[]);
create function arr_in_comp(with_arr) returns int language plphp as $$
	return array_sum($args[0]['vals']);
$$;
select arr_in_comp(row('x', array[5, 6, 7])::with_arr);

drop function arr_in_row(), arr_in_cursor(), arr_in_comp(with_arr), arr_trig();
drop type with_arr;
drop table arrt;
