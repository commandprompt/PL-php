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
