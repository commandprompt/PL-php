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
CREATE or replace FUNCTION trigfunc_ins() RETURNS trigger LANGUAGE plphp AS $$
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

	
