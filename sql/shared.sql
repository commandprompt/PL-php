--
-- $_SHARED test
--

CREATE OR REPLACE FUNCTION php_set_shared (text, text) RETURNS text AS $$
  global $_SHARED;
  $_SHARED[$args[0]] = $args[1];
  return 'ok';
$$ LANGUAGE plphp;

CREATE OR REPLACE FUNCTION php_get_shared (text) RETURNS text AS $$
  global $_SHARED;
  return $_SHARED[$args[0]];
$$ LANGUAGE plphp;

SELECT php_set_shared('first', 'hello plphp');
SELECT php_get_shared('first');

SELECT php_get_shared('second');

SELECT php_set_shared('third', 'hello again!');
SELECT php_set_shared('third', 'say goodbye');
SELECT php_get_shared('third');

CREATE OR REPLACE FUNCTION php_get_shared_ary (text) RETURNS text[] AS $$
  global $_SHARED;
  return $_SHARED[$args[0]];
$$ LANGUAGE plphp;

SELECT php_set_shared('fourth', $${'hip', 'hooray'}$$);
SELECT php_get_shared('fourth');
SELECT php_get_shared_ary('fourth');
SELECT (php_get_shared_ary('fourth'))[1];
SELECT (php_get_shared_ary('fourth'))[1];
SELECT (php_get_shared_ary('fourth'))[2];
