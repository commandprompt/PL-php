--
-- Create tables and functions
--
CREATE TABLE test (
	i int,
	v varchar
);

CREATE TYPE __testrowphp AS (f1 integer, f2 text, f3 text);

CREATE OR REPLACE FUNCTION php_set (integer) RETURNS SETOF __testrowphp AS '
$ret[0][f1] = $args[0];
$ret[0][f2] = "hello";
$ret[0][f3] = "world";

$ret[1][f1] = 2 * $args[0];
$ret[1][f2] = "hello";
$ret[1][f3] = "postgres";

$ret[2][f1] = 3 * $args[0];
$ret[2][f2] = "hello";
$ret[2][f3] = "plphp";
return $ret;
' LANGUAGE 'plphp';

CREATE OR REPLACE FUNCTION php_set_i (integer) RETURNS SETOF integer AS '
$ret[0]=$args[0];
$ret[1]=2*$args[0];
$ret[2]=3*$args[0];
return $ret;
' LANGUAGE 'plphp';


INSERT INTO employee VALUES('plphp', 11, 22);

CREATE FUNCTION empcomp(employee) RETURNS integer AS '
    return $args[0][''basesalary''] + $args[0][''bonus''];
' LANGUAGE plphp;


CREATE OR REPLACE FUNCTION php_arr_in(integer, integer[][]) returns integer AS '
return $args[1][0][1];
' language plphp;
