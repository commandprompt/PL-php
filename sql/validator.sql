--
-- Test the validator
--

CREATE FUNCTION invalid() RETURNS void LANGUAGE plphp AS $$
	asdasd
$$;

CREATE FUNCTION invalid() RETURNS void LANGUAGE plphp AS $$
	{
$$;

CREATE OR REPLACE FUNCTION valid() RETURNS void LANGUAGE plphp AS $$
	array_append();
$$;
select valid();

CREATE FUNCTION valid2() RETURNS void LANGUAGE plphp AS $$
	return array();
$$;
select valid2();
