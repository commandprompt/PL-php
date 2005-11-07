--
-- array returning test
--

CREATE FUNCTION php_array() RETURNS integer[][] AS $$
 $ret1[0]=1;
 $ret1[1]=3;
 $ret1[2]=5;

 $ret2[0]=2;
 $ret2[1]=4;
 $ret2[2]=6;

 $arr[0]=$ret1;
 $arr[1]=$ret2;
 return $arr;

$$ language plphp;

SELECT php_array();

CREATE FUNCTION php_array_txt() RETURNS text[]
LANGUAGE plphp AS $$
 return array('hello', 'world', 'this is a very long string, does it even work?');
$$;

SELECT php_array_txt();
