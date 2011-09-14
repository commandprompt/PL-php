--
-- Test Set-returning functions
--

-- Test a function returning nothing
create function retnothing() returns setof record
language plphp as $$ ; $$;
select * from retnothing() as a(a int);

create type nothing as (a int[]);
create function retnothing2() returns setof nothing
language plphp as $$
pg_raise('notice', 'hello world');
return;
pg_raise('error', 'fatal error');
$$;
select * from retnothing2();

-- Return an anonymous record, resolving the type at query time
create function retset() returns setof record
language plphp as $$
	return_next(array(1, 'hello'));
	return_next(array(2, 'world'));
	return_next(array(3, 'plphp rocks!'));
$$;
select * from retset() as f(a int, b text);

-- Try a predeclared type
create type type_foo as (a int, b text);
create function retset2() returns setof type_foo
language plphp as $$
	return_next(array(1, 'hello'));
	return_next(array(2, 'world'));
	return_next(array(3, 'plphp rocks!'));
$$;
select * from retset2();

-- Return a scalar.  Note no array() construct
create function retset3(int, int) returns setof int
language plphp as $$
	for ($i = 1; $i <= $args[0]; $i++) {
		return_next($args[1] * $i);
	}
$$;
select * from retset3(10, 3);

-- Same as above but with array()
create function retset4(int, int) returns setof int
language plphp as $$
	for ($i = 1; $i <= $args[0]; $i++) {
		return_next(array($args[1] * $i));
	}
$$;
select * from retset4(10, 3);


-- Do we support OUT params?
create function retset5(out int, out int, int, int)
language plphp as $$
	for ($i = 1; $i <= $args[2]; $i++) {
		return_next($args[3] * $i);
	}
$$;
select * from retset5(5, 2);

-- Try to return setof array
create or replace function retset6(int, int) returns setof int[]
language plphp as $$
	for ($i = 1; $i <= $args[0]; $i++) {
		return_next(array(array($args[1] * $i, $i)));
	}
$$;
select * from retset6(10, 2);

-- What happens if we use the "avoid outer array hack" here?
create or replace function retset7(int, int) returns setof int[]
language plphp as $$
	for ($i = 1; $i <= $args[0]; $i++) {
		return_next(array($args[1] * $i, $i));
	}
$$;
select * from retset7(10, 2);

create type dual_int_arr as (label text, a int[], b int[]);
create or replace function retset8(int, int) returns setof dual_int_arr
language plphp as $$
	for ($i = 1; $i <= $args[0]; $i++) {
		return_next(
		array('one', array($args[1] * $i, $i),
			array(
				array($args[1] * $i * 2, $i * 2, $i * $i),
				array($args[1] * $i * 3, $i * 3, $i * 4)
			), 'foo'
		));
	}
$$;
select * from retset8(10, 2);

create table large_table (a int);
insert into large_table select * from generate_series(1, 10000);
create or replace function duplicate(text, text) returns setof int
language plphp as $$
  $r = spi_exec("select $args[1] as a from $args[0]");
  while ($row = spi_fetch_row($r)) {
  	return_next($row['a'] * 2);
  }
$$;

create table duplicated as select * from duplicate('large_table', 'a');
select min(duplicate) from duplicated;
select count(*) from duplicated;
