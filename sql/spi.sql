create or replace function test_spi_1(int) returns int language plphp as $$
	$query = "select generate_series(1, $args[0])";
	$result = spi_exec_query($query);

	$msg = "processed: ".$result['processed']. " ";
	$msg .= "status: ".$result['status'];
	pg_raise('notice', $msg);

	$a = 0;
	while ($row = spi_fetch_row($result))
		$a += $row['generate_series'];

	return $a;
$$;

select test_spi_1(1);
select test_spi_1(5);
select test_spi_1(100);
select test_spi_1(142857);
