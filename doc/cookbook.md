# PL/php cookbook

Practical, self-contained recipes showing where PL/php earns its keep over
`plpgsql`: PHP's standard library. Everything in the first section is run by
the `cookbook` regression test on every supported PostgreSQL version: if it's
on this page, it works.

The embedded interpreter ships with `json`, `pcre`, `openssl`, `sodium`,
`hash` (including `password_hash`), `filter`, `iconv`, `zlib`, DOM/SimpleXML,
sockets, and `fileinfo`. Remember PL/php is **untrusted and superuser-only**:
these functions run with the full privileges of the server's OS user (see
[Security](plphp.md#security)).

## Tested recipes

### Validate data with `filter_var`

No hand-maintained email regex. `IMMUTABLE STRICT` makes it usable in CHECK
constraints and indexes:

```sql
CREATE FUNCTION is_valid_email(text) RETURNS boolean
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return filter_var($args[0], FILTER_VALIDATE_EMAIL) !== false;
$$;

CREATE TABLE subscribers (
    email text PRIMARY KEY CHECK (is_valid_email(email))
);
```

`FILTER_VALIDATE_URL`, `FILTER_VALIDATE_IP` (with `FILTER_FLAG_IPV6`,
`FILTER_FLAG_NO_PRIV_RANGE`, ...) and `FILTER_VALIDATE_DOMAIN` work the same
way.

### bcrypt passwords without pgcrypto

```sql
CREATE FUNCTION hash_password(text) RETURNS text
LANGUAGE plphp STRICT AS $$
    return password_hash($args[0], PASSWORD_BCRYPT);
$$;

CREATE FUNCTION check_password(plain text, hashed text) RETURNS boolean
LANGUAGE plphp STRICT AS $$
    return password_verify($args[0], $args[1]);
$$;

SELECT check_password('s3cret', hash_password('s3cret'));   -- true
```

`password_hash` salts automatically; store its output as-is. `PASSWORD_ARGON2I`
is available too if libargon2 was compiled in.

### HMAC-signed tokens

Issue and verify tamper-proof tokens with a server-side secret, no extension
needed:

```sql
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
```

`verify_token` returns the payload when the signature checks out and NULL
otherwise, and uses constant-time `hash_equals`.

### Reshape JSON recursively

Deeply redact keys anywhere in a document. Awkward with `jsonb` operators,
three lines of PHP:

```sql
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
-- {"user": "jd", "profile": {"city": "Olympia", "api_key": "[redacted]"}, "password": "[redacted]"}
```

### Extract with a regex, return a set

`preg_match_all` + `return_next` turns any regex into a set-returning
function:

```sql
CREATE FUNCTION extract_urls(text) RETURNS SETOF text
LANGUAGE plphp IMMUTABLE STRICT AS $$
    preg_match_all('~https?://[^\s<>")\']+~', $args[0], $m);
    foreach ($m[0] as $url)
        return_next($url);
    return;
$$;

SELECT extract_urls('see https://example.com/a and http://foo.bar/b?x=1.');
```

### A generic audit trigger

One trigger function for any table: diffs OLD against NEW into `jsonb`, and
caches its INSERT plan in `$_SHARED` so it is prepared once per session:

```sql
CREATE TABLE audit_log (tab text, op text, changed jsonb);

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
    FOR EACH ROW EXECUTE FUNCTION audit();
```

### Batch processing with periodic commits

In a procedure `CALL`ed non-atomically, commit every batch so long jobs don't
hold one giant transaction. Note the loop re-queries rather than holding a
cursor: **portals do not survive `spi_commit`** (a `spi_fetchrow` after commit
just returns `false`).

```sql
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

CALL process_queue(1000);
```

### Stream a big scan, stop early

A cursor (`spi_query`) reads rows one at a time in constant memory. Close it
as soon as you have your answer instead of materializing millions of rows:

```sql
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
```

### Read a CSV file into rows

Server-side file ETL without `COPY`'s format restrictions: `fgetcsv` handles
quoting/escaping, and `RETURNS TABLE` + `return_next(array)` does the rest:

```sql
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

SELECT * FROM load_csv('/srv/import/products.csv');
```

### Shred XML into rows

```sql
CREATE FUNCTION xml_items(doc xml)
RETURNS TABLE (name text, qty int)
LANGUAGE plphp STRICT AS $$
    $doc = simplexml_load_string($args[0]);
    foreach ($doc->item as $item)
        return_next(array((string) $item['name'], (int) $item['qty']));
    return;
$$;

SELECT * FROM xml_items('<order><item name="bolt" qty="9"/><item name="nut" qty="4"/></order>');
```

### Compress large text into bytea

```sql
CREATE FUNCTION gz(text) RETURNS bytea
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return "\\x" . bin2hex(gzcompress($args[0], 9));
$$;

CREATE FUNCTION gunz(bytea) RETURNS text
LANGUAGE plphp IMMUTABLE STRICT AS $$
    return gzuncompress(hex2bin(substr($args[0], 2)));
$$;

SELECT gunz(gz(long_text)) = long_text FROM documents;   -- true
```

(PostgreSQL TOAST already compresses transparently; this is for when you want
the compressed bytes explicitly, to hand to a client or use with `SET STORAGE
EXTERNAL`.)

## Doc-only recipes (side effects: use with care)

These work but are deliberately not in the regression test: they touch the
outside world, and they run **inside your transaction on the backend**. A slow
webhook blocks the INSERT that fired it, and a rolled-back transaction does
*not* un-send an HTTP request or an email. For anything latency-sensitive,
write to a queue table instead and let a worker drain it.

### HTTP webhook from a trigger

No curl needed: PHP's HTTP stream wrapper does POSTs, and a short timeout is
mandatory:

```sql
CREATE FUNCTION notify_webhook() RETURNS trigger LANGUAGE plphp AS $$
    $ctx = stream_context_create(array("http" => array(
        "method"  => "POST",
        "header"  => "Content-Type: application/json",
        "content" => json_encode($_TD['new']),
        "timeout" => 2,
    )));
    @file_get_contents("https://hooks.example.com/row-changed", false, $ctx);
    return;   -- never fail the DML because the webhook is down
$$;
```

### Email notification

```sql
CREATE FUNCTION mail_alert(subject text, body text) RETURNS void
LANGUAGE plphp STRICT AS $$
    mail("ops@example.com", $args[0], $args[1]);
$$;
```

Requires a working `sendmail` on the database server.

### Slugify with transliteration

`iconv`'s `//TRANSLIT` is locale-dependent, so results can vary between
systems: good for display, not for keys that must be stable everywhere:

```sql
CREATE FUNCTION slugify(text) RETURNS text LANGUAGE plphp STRICT AS $$
    $s = iconv("UTF-8", "ASCII//TRANSLIT//IGNORE", $args[0]);
    $s = strtolower(preg_replace("/[^A-Za-z0-9]+/", "-", $s));
    return trim($s, "-");
$$;
```
