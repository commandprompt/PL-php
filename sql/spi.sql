create or replace function test_spi_1(int) returns int language plphp as $$
	$query = "select generate_series(1, $args[0])";
	$result = spi_exec($query);

	$msg = "processed: " . spi_processed($result) . " ";
	$msg .= "status: " . spi_status($result);

	pg_raise('notice', $msg);

	$a = 0;
	while ($row = spi_fetch_row($result))
		$a += $row['generate_series'];

	pg_raise('notice', "total sum is " . $a);

	return $a;
$$;

select test_spi_1(1);
select test_spi_1(5);
select test_spi_1(100);
select test_spi_1(142857);

create or replace function test_spi_2() returns void language plphp as $$
$query = "select 1 as a union all select 2";
$res1 = spi_exec($query);

$query = "select 3 as b union all select 4";
$res2 = spi_exec($query);

while ($r1 = spi_fetch_row($res1))
{
	spi_rewind($res2);
	while ($r2 = spi_fetch_row($res2))
	{
		$msg = sprintf("got values %d and %d", $r1['a'], $r2['b']);
		pg_raise('notice', $msg);
	}
}
$$;

select test_spi_2();

create function test_spi_limit() returns void language plphp as $$
$query = "select 1 as a union all select 2 union all select 3";
$res = spi_exec($query, 2);
while ($r = spi_fetch_row($res))
{
	pg_raise('notice', "got value " . $r['a']);
}
$$;

select test_spi_limit();
