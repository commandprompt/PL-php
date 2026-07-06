-- Every tested recipe from doc/cookbook.md, verbatim.  If this test passes,
-- the cookbook's "Tested recipes" section is honest.

-- Validate data with filter_var
CREATE FUNCTION is_valid_email(text) RETURNS boolean
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return filter_var($args[0], FILTER_VALIDATE_EMAIL) !== false;
$$;
SELECT is_valid_email('jd@example.com');
SELECT is_valid_email('not-an-email');
CREATE TABLE subscribers (
    email text PRIMARY KEY CHECK (is_valid_email(email))
);
INSERT INTO subscribers VALUES ('jd@example.com');
INSERT INTO subscribers VALUES ('bogus@@nope');

-- bcrypt passwords without pgcrypto
CREATE FUNCTION hash_password(text) RETURNS text
LANGUAGE plphp STRICT AS $$
    return password_hash($args[0], PASSWORD_BCRYPT);
$$;
CREATE FUNCTION check_password(plain text, hashed text) RETURNS boolean
LANGUAGE plphp STRICT AS $$
    return password_verify($args[0], $args[1]);
$$;
SELECT check_password('s3cret', hash_password('s3cret'));
SELECT check_password('wrong', hash_password('s3cret'));

-- HMAC-signed tokens
CREATE FUNCTION sign_token(payload text, secret text) RETURNS text
LANGUAGE plphp STRICT AS $$
    return $args[0] . "." . hash_hmac("sha256", $args[0], $args[1]);
$$;
CREATE FUNCTION verify_token(token text, secret text) RETURNS text
LANGUAGE plphp STRICT AS $$
    $pos = strrpos($args[0], ".");
    if ($pos === false)
        return null;
    $payload = substr($args[0], 0, $pos);
    $mac     = substr($args[0], $pos + 1);
    return hash_equals(hash_hmac("sha256", $payload, $args[1]), $mac)
        ? $payload : null;
$$;
SELECT sign_token('user=jd;exp=2038-01-19', 'hunter2');
SELECT verify_token(sign_token('user=jd;exp=2038-01-19', 'hunter2'), 'hunter2');
SELECT verify_token('user=jd;exp=2038-01-19.deadbeef', 'hunter2') IS NULL AS tampered;

-- Reshape JSON recursively
CREATE FUNCTION jsonb_redact(doc jsonb, keys text[]) RETURNS jsonb
LANGUAGE plphp STRICT AS $$
    $doc  = json_decode($args[0], true);
    $keys = $args[1];
    $walk = function (&$node) use (&$walk, $keys) {
        foreach ($node as $k => &$v) {
            if (in_array((string) $k, $keys, true))
                $v = "[redacted]";
            elseif (is_array($v))
                $walk($v);
        }
    };
    $walk($doc);
    return json_encode($doc);
$$;
SELECT jsonb_redact(
    '{"user":"jd","password":"x","profile":{"api_key":"y","city":"Olympia"}}',
    ARRAY['password', 'api_key']);

-- Extract with a regex, return a set
CREATE FUNCTION extract_urls(text) RETURNS SETOF text
LANGUAGE plphp IMMUTABLE STRICT AS $$
    preg_match_all('~https?://[^\s<>")\']+~', $args[0], $m);
    foreach ($m[0] as $url)
        return_next($url);
    return;
$$;
SELECT extract_urls('see https://example.com/a and http://foo.bar/b?x=1 today');

-- A generic audit trigger
CREATE TABLE audit_log (tab text, op text, changed jsonb);
CREATE TABLE accounts (id int primary key, owner text, balance int);
CREATE FUNCTION audit() RETURNS trigger LANGUAGE plphp AS $$
    global $_SHARED;

    $old = $_TD['event'] == 'INSERT' ? array() : $_TD['old'];
    $new = $_TD['event'] == 'DELETE' ? array() : $_TD['new'];

    $diff = array();
    foreach ($new as $k => $v)
        if (!array_key_exists($k, $old) || $old[$k] !== $v)
            $diff[$k] = array("from" => $old[$k] ?? null, "to" => $v);
    foreach ($old as $k => $v)
        if (!array_key_exists($k, $new))
            $diff[$k] = array("from" => $v, "to" => null);

    if (!isset($_SHARED['audit_plan']))
        $_SHARED['audit_plan'] = spi_prepare(
            'insert into audit_log values ($1, $2, $3)',
            'text', 'text', 'jsonb');
    spi_exec_prepared($_SHARED['audit_plan'],
                      $_TD['relname'], $_TD['event'],
                      json_encode($diff ?: new stdClass()));
    return;
$$;
CREATE TRIGGER t_audit AFTER INSERT OR UPDATE OR DELETE ON accounts
    FOR EACH ROW EXECUTE PROCEDURE audit();
INSERT INTO accounts VALUES (1, 'jd', 100);
UPDATE accounts SET balance = 250 WHERE id = 1;
UPDATE accounts SET balance = 250 WHERE id = 1;   -- no-op change
DELETE FROM accounts WHERE id = 1;
SELECT * FROM audit_log;

-- Batch processing with periodic commits
CREATE TABLE queue (id int primary key, done boolean default false);
INSERT INTO queue SELECT g FROM generate_series(1, 10) g;
CREATE PROCEDURE process_queue(batch_size int) LANGUAGE plphp AS $$
    while (true) {
        $r = spi_exec("update queue set done = true
                       where id in (select id from queue
                                    where not done
                                    order by id limit {$args[0]})");
        if (spi_processed($r) == 0)
            break;
        pg_raise('notice', 'processed ' . spi_processed($r) . ' rows');
        spi_commit();
    }
$$;
CALL process_queue(4);
SELECT count(*) FILTER (WHERE done) AS done, count(*) AS total FROM queue;

-- Stream a big scan, stop early
CREATE TABLE ledger (id int primary key, amount numeric);
INSERT INTO ledger SELECT g, 10 FROM generate_series(1, 100000) g;
CREATE FUNCTION first_exceeding(threshold numeric) RETURNS int
LANGUAGE plphp STRICT AS $$
    $c = spi_query("select id, amount from ledger order by id");
    $total = 0;
    while ($row = spi_fetchrow($c)) {
        $total += $row['amount'];
        if ($total > $args[0]) {
            spi_cursor_close($c);
            return $row['id'];
        }
    }
    return null;
$$;
SELECT first_exceeding(255);

-- Read a CSV file into rows
CREATE FUNCTION write_demo_csv(path text) RETURNS void LANGUAGE plphp STRICT AS $$
    file_put_contents($args[0],
        "bolt,\"14 mm, zinc\",9\nnut,\"quoted \"\"name\"\"\",4\n");
$$;
CREATE FUNCTION load_csv(path text)
RETURNS TABLE (a text, b text, c text)
LANGUAGE plphp STRICT AS $$
    $fh = fopen($args[0], "r");
    if ($fh === false)
        pg_raise('error', "cannot open {$args[0]}");
    while (($rec = fgetcsv($fh, null, ",", "\"", "\\")) !== false)
        return_next($rec);
    fclose($fh);
    return;
$$;
SELECT write_demo_csv('/tmp/plphp_cookbook.csv');
SELECT * FROM load_csv('/tmp/plphp_cookbook.csv');

-- Shred XML into rows
CREATE FUNCTION xml_items(doc xml)
RETURNS TABLE (name text, qty int)
LANGUAGE plphp STRICT AS $$
    $doc = simplexml_load_string($args[0]);
    foreach ($doc->item as $item)
        return_next(array((string) $item['name'], (int) $item['qty']));
    return;
$$;
SELECT * FROM xml_items('<order><item name="bolt" qty="9"/><item name="nut" qty="4"/></order>');

-- Compress large text into bytea
CREATE FUNCTION gz(text) RETURNS bytea
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return "\\x" . bin2hex(gzcompress($args[0], 9));
$$;
CREATE FUNCTION gunz(bytea) RETURNS text
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return gzuncompress(hex2bin(substr($args[0], 2)));
$$;
SELECT gunz(gz(repeat('squeeze me, ', 500))) = repeat('squeeze me, ', 500) AS roundtrip,
       octet_length(gz(repeat('squeeze me, ', 500))) < 100 AS compressed;

-- clean up
DO $$ @unlink('/tmp/plphp_cookbook.csv'); $$ LANGUAGE plphp;
DROP TABLE subscribers, audit_log, accounts, queue, ledger;
