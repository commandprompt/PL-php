--
-- Test trigger capabilities
--

CREATE FUNCTION trigfunc() RETURNS trigger LANGUAGE plphp AS $$
	$extra = "";
	if ($_TD['level'] == 'ROW') {
		if ($_TD['event'] == 'DELETE' || $_TD['event'] == 'UPDATE') {
			$extra .= " old val: ". $_TD['old']['a'];
		}
		if ($_TD['event'] == 'INSERT' || $_TD['event'] == 'UPDATE') {
			$extra .= " new val: ". $_TD['new']['a'];
		}
	}
	
	pg_raise('notice', "trigger: ${_TD['name']} table: ${_TD['relname']}, ".
		 "level: ${_TD['level']} event: ${_TD['when']} ${_TD['event']}$extra");
$$;

create table trigfunc_table (a int);
create trigger trigtest0 before insert on trigfunc_table for each row execute procedure trigfunc();
create trigger trigtest1 before update on trigfunc_table for each row execute procedure trigfunc();
create trigger trigtest2 before delete on trigfunc_table for each row execute procedure trigfunc();

create trigger trigtest3 before insert on trigfunc_table for each statement execute procedure trigfunc();
create trigger trigtest4 before update on trigfunc_table for each statement execute procedure trigfunc();
create trigger trigtest5 before delete on trigfunc_table for each statement execute procedure trigfunc();

create trigger trigtest6 after insert on trigfunc_table for each row execute procedure trigfunc();
create trigger trigtest7 after update on trigfunc_table for each row execute procedure trigfunc();
create trigger trigtest8 after delete on trigfunc_table for each row execute procedure trigfunc();

create trigger trigtest9 after insert on trigfunc_table for each statement execute procedure trigfunc();
create trigger trigtest10 after update on trigfunc_table for each statement execute procedure trigfunc();
create trigger trigtest11 after delete on trigfunc_table for each statement execute procedure trigfunc();

-- insert two rows with a=1 so we can see the UPDATE per-row trigger
-- firing twice
insert into trigfunc_table values (1);
insert into trigfunc_table values (1);
insert into trigfunc_table values (2);

update trigfunc_table set a = 2 where a = 1;

delete from trigfunc_table where a = 2;

-- Test SKIP and MODIFY
CREATE FUNCTION trigfunc_ins() RETURNS trigger LANGUAGE plphp AS $$
	$a = $_TD['new']['a'];
	if ($a == 1) {
		return 'SKIP';
	} else if ($a == 2) {
		$_TD['new']['a'] = 3;
		return 'MODIFY';
	} else if ($a == 4) {
		return NULL;
	} else if ($a == 5) {
		return 'gibberish';
	} else {
		pg_raise('error', "value a=$a is not allowed");
	}
$$;

create table b (a int);
create trigger trigtest12 before insert on b for each row execute procedure trigfunc_ins();


insert into b values (1);
insert into b values (2);
insert into b values (3);
insert into b values (4);
insert into b values (5);
insert into b values (6);
select * from b;

-- Add an additional attribute to NEW
CREATE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new']['a'] = 'foo';
	return 'MODIFY';
$$;
CREATE TABLE trigfunc_test1 (b int);
CREATE TRIGGER trig_test1 BEFORE INSERT ON trigfunc_test1
FOR EACH ROW EXECUTE PROCEDURE trigfunc_test1();
-- NEW has an incorrect number of keys
INSERT INTO trigfunc_test1 VALUES (1);

-- Create NEW as a new array, with a single attribute that's not the expected
-- one
CREATE OR REPLACE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new'] = array('a' => 'foo');
	return 'MODIFY';
$$;
-- Invalid attribute "a" in NEW
INSERT INTO trigfunc_test1 VALUES (1);

-- Create NEW as the correct array, but with a NULL value
CREATE OR REPLACE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new'] = array('b' => NULL);
	return 'MODIFY';
$$;
INSERT INTO trigfunc_test1 VALUES (1);
-- should have inserted a NULL
SELECT b IS NULL FROM trigfunc_test1;
DELETE FROM trigfunc_test1;

-- Return an empty array
CREATE OR REPLACE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new'] = array();
	return 'MODIFY';
$$;
INSERT INTO trigfunc_test1 VALUES (1);

-- Make "b" a literal of an incorrect type
CREATE OR REPLACE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new'] = array('b' => 'foobar');
	return 'MODIFY';
$$;
INSERT INTO trigfunc_test1 VALUES (1);

-- Make "b" an array
CREATE OR REPLACE FUNCTION trigfunc_test1() RETURNS trigger LANGUAGE plphp AS $$
	$_TD['new'] = array('b' => array(1, 2, 3));
	return 'MODIFY';
$$;
INSERT INTO trigfunc_test1 VALUES (1);

-- What happens if we create a table whose columns are numbers?
-- Does PHP regular array handling work?
CREATE TABLE numbers ("0" int, "1" text, "2" float, "3" numeric[]);
CREATE OR REPLACE FUNCTION numbers_trig() RETURNS TRIGGER LANGUAGE plphp AS $$
	$_TD['new'] = array(5, 'hello', 1.4142,
		array(-123, 14 * 3, 142857 * 2));
	RETURN 'MODIFY';
$$;
CREATE TRIGGER numbers_trig BEFORE INSERT ON numbers
FOR EACH ROW EXECUTE PROCEDURE numbers_trig();
INSERT INTO numbers DEFAULT VALUES;
SELECT * FROM numbers;

-- Now use strings with the names of the columns
CREATE OR REPLACE FUNCTION numbers_trig() RETURNS TRIGGER LANGUAGE plphp AS $$
	$_TD['new'] = array("0" => 42, "1" => 'hello new jersey', "2" => 3.14,
		"3" => array(array(1,7),array(2,3),array(4,8),array(5,6)));
	RETURN 'MODIFY';
$$;
INSERT INTO numbers DEFAULT VALUES;
SELECT * FROM numbers;
