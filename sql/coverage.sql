-- Coverage for previously-untested paths: start_proc failure, DML/utility
-- SPI results, trigger arguments, cursor/transaction interactions, UTF-8,
-- TOASTed values, PHP exception handling, DO runtime errors.

-- start_proc failure: the first call of the session fails, but the session
-- is already marked initialized, so after clearing the GUC things work.
CREATE FUNCTION cov_ident(int) RETURNS int LANGUAGE plphp AS $$
	return $args[0];
$$;
SET plphp.start_proc = 'cov_no_such_fn';
SELECT cov_ident(1);
RESET plphp.start_proc;
SELECT cov_ident(1);

-- DML and utility statements through spi_exec: status and row counts
CREATE FUNCTION cov_dml() RETURNS text LANGUAGE plphp AS $$
	$out = array();
	$r = spi_exec("create table cov_t (a int, b text)");
	$out[] = spi_status($r);
	$r = spi_exec("insert into cov_t select g, 'row ' || g from generate_series(1, 3) g");
	$out[] = spi_status($r) . ":" . spi_processed($r);
	$r = spi_exec("update cov_t set b = b || '!' where a < 3");
	$out[] = spi_status($r) . ":" . spi_processed($r);
	$r = spi_exec("delete from cov_t where a = 1");
	$out[] = spi_status($r) . ":" . spi_processed($r);
	// fetching from a non-SELECT result is refused politely
	$row = spi_fetch_row($r);
	$out[] = $row === false ? "no-fetch" : "fetched?";
	return implode(" ", $out);
$$;
SELECT cov_dml();
SELECT * FROM cov_t ORDER BY a;

-- spi_rewind on a non-SELECT result is harmless
CREATE FUNCTION cov_rewind_dml() RETURNS text LANGUAGE plphp AS $$
	$r = spi_exec("insert into cov_t values (9, 'nine')");
	spi_rewind($r);
	return spi_status($r) . ":" . spi_processed($r);
$$;
SELECT cov_rewind_dml();

-- Trigger arguments: $_TD['args'] carries CREATE TRIGGER's literal arguments
CREATE FUNCTION cov_trigargs() RETURNS trigger LANGUAGE plphp AS $$
	pg_raise('notice', "trigger args: argc=" . $_TD['argc']
		. " [" . implode(",", $_TD['args']) . "]");
	return;
$$;
CREATE TRIGGER cov_trg BEFORE INSERT ON cov_t
	FOR EACH ROW EXECUTE PROCEDURE cov_trigargs('alpha', '42');
INSERT INTO cov_t VALUES (10, 'ten');
DROP TRIGGER cov_trg ON cov_t;

-- A cursor does not survive spi_commit: the portal dies with the
-- transaction and further fetches just return false.
CREATE PROCEDURE cov_cursor_commit() LANGUAGE plphp AS $$
	$c = spi_query("select a from cov_t order by a");
	$row = spi_fetchrow($c);
	pg_raise('notice', "before commit: a=" . $row['a']);
	spi_commit();
	$row = spi_fetchrow($c);
	pg_raise('notice', "after commit: " . ($row === false ? "false" : "a row"));
$$;
CALL cov_cursor_commit();

-- A cursor opened inside a rolled-back subtransaction() disappears with it
CREATE FUNCTION cov_cursor_subxact() RETURNS text LANGUAGE plphp AS $$
	$c = null;
	try {
		subtransaction(function() use (&$c) {
			$c = spi_query("select 1 as x");
			throw new Exception("abort the subxact");
		});
	} catch (Exception $e) {
		pg_raise('notice', "caught: " . $e->getMessage());
	}
	$row = spi_fetchrow($c);
	return $row === false ? "cursor gone" : "cursor survived";
$$;
SELECT cov_cursor_subxact();

-- Multibyte UTF-8 round trip (strlen counts bytes)
CREATE FUNCTION cov_utf8(text) RETURNS text LANGUAGE plphp AS $$
	return $args[0] . " (" . strlen($args[0]) . " bytes)";
$$;
SELECT cov_utf8('héllo wörld 日本語');

-- A TOAST-sized value passes through intact
CREATE FUNCTION cov_toast(text) RETURNS text LANGUAGE plphp AS $$
	return strlen($args[0]) . ":" . md5($args[0]);
$$;
SELECT cov_toast(repeat('abcdefghij', 1000));

-- PHP exceptions are catchable in plain function code
CREATE FUNCTION cov_catch() RETURNS text LANGUAGE plphp AS $$
	try {
		throw new RuntimeException("boom");
	} catch (RuntimeException $e) {
		return "caught " . $e->getMessage();
	}
$$;
SELECT cov_catch();

-- A DO block can fail at runtime (not just at parse time)
DO $$ pg_raise('error', 'do block runtime error'); $$ LANGUAGE plphp;

DROP TABLE cov_t;
