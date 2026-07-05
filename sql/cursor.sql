-- cursor-streaming SPI: spi_query / spi_fetchrow / spi_cursor_close /
-- spi_query_prepared

-- basic streaming loop: constant-memory iteration over a result set
create function test_cursor_basic(int) returns int language plphp as $$
	$cursor = spi_query("select generate_series(1, $args[0]) as g");
	$sum = 0;
	while ($row = spi_fetchrow($cursor))
		$sum += $row['g'];
	return $sum;
$$;

select test_cursor_basic(1);
select test_cursor_basic(5);
select test_cursor_basic(1000);

-- the cursor is closed automatically at exhaustion: further fetches
-- just return false
create function test_cursor_exhausted() returns text language plphp as $$
	$cursor = spi_query("select 1 as a");
	$row = spi_fetchrow($cursor);
	pg_raise('notice', "first fetch: " . $row['a']);
	$row = spi_fetchrow($cursor);
	pg_raise('notice', "second fetch is " . ($row === false ? "false" : "a row"));
	$row = spi_fetchrow($cursor);
	pg_raise('notice', "third fetch is " . ($row === false ? "false" : "a row"));
	return "ok";
$$;

select test_cursor_exhausted();

-- early close: abandon a cursor before exhaustion; fetching afterwards
-- returns false, and closing again is harmless
create function test_cursor_close() returns text language plphp as $$
	$cursor = spi_query("select generate_series(1, 1000000) as g");
	$row = spi_fetchrow($cursor);
	pg_raise('notice', "got first row: " . $row['g']);
	spi_cursor_close($cursor);
	$row = spi_fetchrow($cursor);
	pg_raise('notice', "fetch after close is " . ($row === false ? "false" : "a row"));
	spi_cursor_close($cursor);
	return "ok";
$$;

select test_cursor_close();

-- fetching from a cursor that never existed returns false
create function test_cursor_bogus() returns text language plphp as $$
	$row = spi_fetchrow("no such cursor");
	return $row === false ? "false" : "a row";
$$;

select test_cursor_bogus();

-- streaming from a prepared plan with arguments
create function test_cursor_prepared(int, int) returns int language plphp as $$
	$plan = spi_prepare("select generate_series($1, $2) as g", "int4", "int4");
	$cursor = spi_query_prepared($plan, $args[0], $args[1]);
	$sum = 0;
	while ($row = spi_fetchrow($cursor))
		$sum += $row['g'];
	spi_freeplan($plan);
	return $sum;
$$;

-- 3 + 4 + 5 = 12
select test_cursor_prepared(3, 5);

-- two cursors interleaved
create function test_cursor_interleave() returns int language plphp as $$
	$c1 = spi_query("select 1 as a union all select 2");
	$c2 = spi_query("select 30 as b union all select 40");
	$ret = 0;
	while (($r1 = spi_fetchrow($c1)) && ($r2 = spi_fetchrow($c2)))
	{
		pg_raise('notice', sprintf("got values %d and %d", $r1['a'], $r2['b']));
		$ret += $r1['a'] + $r2['b'];
	}
	return $ret;
$$;

-- 1 + 30 + 2 + 40 = 73
select test_cursor_interleave();

-- an error in the query is reported and the function can run again
create function test_cursor_error(int) returns int language plphp as $$
	$cursor = spi_query("select 1 / $args[0] as q");
	$row = spi_fetchrow($cursor);
	return $row['q'];
$$;

select test_cursor_error(1);
select test_cursor_error(0);
select test_cursor_error(1);
