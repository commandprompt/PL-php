--
-- Variable names.
--

-- A function with uses named and unnamed variables

CREATE FUNCTION zero(a int, b int) RETURNS int LANGUAGE plphp AS 
$$
	return $a * $args[1] - $b * $args[0];
$$;

SELECT zero((random() * 100) :: int, (random() * 100) :: int);

-- A function with both named and unnamed vars in declaration

CREATE FUNCTION obscure_concat(prefix text, text) RETURNS text LANGUAGE plphp AS
$$
	return $prefix . $args[1];
$$;

SELECT obscure_concat('Testing can only prove the presence of bugs', ', not their absence. ');

-- Check for a long variable name truncation. This check assumes that NAMEDATALEN is 64.

CREATE FUNCTION foo(areallylooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooongvariablename text)
RETURNS text LANGUAGE plphp AS
'
	$shortvar = substr("areallylooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooongvariablename", 0, 63);
	if ($args[0] != $$shortvar)
		return "Error";
	else
		return "Good";
';

select foo('bar');

-- What if we use non- symbols in variable names 

CREATE FUNCTION php_invalidvarname( check%s%d%s%d%s%d text)
RETURN text LANGUAGE plphp AS
$$
	return "passed" . $args[0];
$$;

select php_invalidvarname('foo');



