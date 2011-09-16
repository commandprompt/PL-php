CREATE SCHEMA plphp;

-- check an SRF with multiple OUT arguments.
CREATE OR REPLACE FUNCTION plphp.multiple_out(number OUT INTEGER, prefered OUT TEXT) RETURNS SETOF RECORD
AS $$
	$result = array(	
		array(0, 'zero'),
		array(1, 'ichi'),
		array(2, 'ni'),
		array(3, 'san'),
		array(4, 'yon'),
		array(5, 'go'),
		array(6, 'roku'),
		array(7, 'nana'),
		array(8, 'hachi'),
		array(9, 'ku'));
	for ($i = 0; $i < 9; $i++) {
		return_next($result[$i]);
	}
	return;
$$ LANGUAGE plphp;

SELECT * FROM plphp.multiple_out(); 

-- check that INOUT and OUT arguments can be used together.
CREATE OR REPLACE FUNCTION plphp.inout(s OUT INTEGER, a INOUT INTEGER, b INOUT INTEGER) 
AS $$
	$s = $a;
	$a = $b;
	$b = $s;
	$s += $a;
$$ LANGUAGE plphp;

SELECT * FROM plphp.inout(24, 42);

-- check TABLE arguments. Make sure that both return_next(array) and return_next()
-- work when TABLE arguments are declared.
CREATE OR REPLACE FUNCTION plphp.table_out(lim INTEGER) RETURNS TABLE(number INTEGER, square INTEGER) AS
$$
	for ($number = 1; $number <= $lim; $number++) {
		$square = $number * $number;
		if ($number % 2)
			return_next();
		else
			return_next(array($number, $square));
	}
$$ LANGUAGE plphp;

SELECT * FROM plphp.table_out(10);

-- check an OUT function returning invalid values 
CREATE OR REPLACE FUNCTION plphp.out_invalid(a OUT INTEGER, "long identifier" OUT INTEGER) AS
$$
 
  return(array(1,2));
$$ LANGUAGE plphp;

SELECT * FROM plphp.out_invalid();

