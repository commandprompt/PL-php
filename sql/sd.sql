--
-- $_SD test: per-function static dictionary
--
-- $_SD persists across calls to the same function within a session, is private
-- to each function (unlike the session-global $_SHARED), and is reset when the
-- function is redefined.
--

-- A counter that lives in $_SD across calls.
CREATE OR REPLACE FUNCTION sd_counter() RETURNS int AS $$
  if (!isset($_SD['n']))
      $_SD['n'] = 0;
  return ++$_SD['n'];
$$ LANGUAGE plphp;

SELECT sd_counter();
SELECT sd_counter();
SELECT sd_counter();

-- A second function has its own, independent $_SD.
CREATE OR REPLACE FUNCTION sd_counter2() RETURNS int AS $$
  if (!isset($_SD['n']))
      $_SD['n'] = 100;
  return ++$_SD['n'];
$$ LANGUAGE plphp;

SELECT sd_counter2();
SELECT sd_counter2();
-- The first counter is unaffected by the second.
SELECT sd_counter();

-- $_SD is private; $_SHARED is shared. Write both from one function...
CREATE OR REPLACE FUNCTION sd_writer(text) RETURNS text AS $$
  global $_SHARED;
  $_SD['secret'] = $args[0];
  $_SHARED['public'] = $args[0];
  return 'ok';
$$ LANGUAGE plphp;

-- ...and read them from another. It sees $_SHARED but not the writer's $_SD.
CREATE OR REPLACE FUNCTION sd_reader() RETURNS text AS $$
  global $_SHARED;
  $sd = isset($_SD['secret']) ? $_SD['secret'] : 'none';
  $sh = isset($_SHARED['public']) ? $_SHARED['public'] : 'none';
  return "sd=$sd shared=$sh";
$$ LANGUAGE plphp;

SELECT sd_writer('xyz');
SELECT sd_reader();

-- Redefining a function resets its $_SD.
SELECT sd_counter();
CREATE OR REPLACE FUNCTION sd_counter() RETURNS int AS $$
  if (!isset($_SD['n']))
      $_SD['n'] = 0;
  return ++$_SD['n'];
$$ LANGUAGE plphp;
SELECT sd_counter();

-- $_SD can hold structured data, e.g. a cached array.
CREATE OR REPLACE FUNCTION sd_accumulate(int) RETURNS int[] AS $$
  if (!isset($_SD['seen']))
      $_SD['seen'] = array();
  $_SD['seen'][] = $args[0];
  return $_SD['seen'];
$$ LANGUAGE plphp;

SELECT sd_accumulate(10);
SELECT sd_accumulate(20);
SELECT sd_accumulate(30);
