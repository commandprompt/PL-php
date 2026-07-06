# PL/php Language Reference

PL/php lets you write PostgreSQL functions and triggers in PHP. This document
describes the programming interface. For build and install instructions see
[`INSTALL`](../INSTALL); for a feature summary see [`README`](../README.md).

Tested on **PostgreSQL 11 through 18** with **PHP 8.1 through 8.4** (embed
SAPI, non-thread-safe).

- [Enabling the language](#enabling-the-language)
- [Writing functions](#writing-functions)
- [Anonymous code blocks (DO)](#anonymous-code-blocks-do)
- [Data type mapping](#data-type-mapping)
- [Composite types and records](#composite-types-and-records)
- [Arguments: IN, OUT, INOUT, TABLE, named](#arguments)
- [Set-returning functions](#set-returning-functions)
- [Trigger functions](#trigger-functions)
- [Event trigger functions](#event-trigger-functions)
- [Database access (SPI)](#database-access-spi)
- [Prepared statements](#prepared-statements)
- [Transaction control](#transaction-control)
- [Subtransactions](#subtransactions)
- [Quoting helpers](#quoting-helpers)
- [Messaging: elog and pg_raise](#messaging-elog-and-pg_raise)
- [Shared data: `$_SHARED`](#shared-data-_shared)
- [Session initialization: modules and start_proc](#session-initialization-modules-and-start_proc)
- [Errors and exceptions](#errors-and-exceptions)
- [Security](#security)

## Enabling the language

```sql
CREATE EXTENSION plphp;
```

PL/php is an untrusted language, so the extension is superuser-only to install
and only superusers can create PL/php functions. See [Security](#security).

## Writing functions

The body of a `LANGUAGE plphp` function is the body of a PHP function. Inside
it you have:

- `$args`: a 0-indexed array of the call arguments.
- `$argc`: the number of declared arguments.
- Return a value with PHP's `return`.

```sql
CREATE FUNCTION php_max(integer, integer) RETURNS integer
STRICT LANGUAGE plphp AS $$
    return $args[0] > $args[1] ? $args[0] : $args[1];
$$;

SELECT php_max(3, 7);   -- 7
```

`STRICT` (a.k.a. `RETURNS NULL ON NULL INPUT`) makes the function return NULL
automatically when any argument is NULL. Without it, a NULL argument arrives as
an unset element, so test with `isset($args[$n])`.

## Anonymous code blocks (DO)

You can run a one-off block of PHP without defining a function, using `DO`:

```sql
DO $$
    $r = spi_exec('select count(*) as n from pg_class');
    $row = spi_fetch_row($r);
    elog('NOTICE', "pg_class has {$row['n']} rows");
$$ LANGUAGE plphp;
```

A `DO` block takes no arguments and returns nothing; use it for procedural
one-offs, migrations, or ad-hoc maintenance. Because PL/php is untrusted, only
superusers can run `DO ... LANGUAGE plphp`.

## Data type mapping

Arguments are converted from their PostgreSQL text representation to PHP values;
return values are converted back using the declared return type's input
function. In practice:

| PostgreSQL                     | PHP value in `$args` | PHP return value        |
|--------------------------------|----------------------|-------------------------|
| integer / numeric / float      | string               | int / float / numeric string |
| text / varchar / etc.          | string               | string                  |
| boolean                        | `"t"` / `"f"` string | `true` / `false`, or `"t"`/`"f"` |
| arrays (e.g. `int[]`)          | PHP array            | PHP array               |
| composite / row / record       | associative array    | associative array       |

Array- and composite-typed *columns* inside rows (in `$_TD['new']`/`['old']`,
rows from `spi_fetch_row`/`spi_fetchrow`, and composite arguments' fields)
also convert structurally, all the way down: arrays become PHP arrays,
composites become associative arrays, in both directions (so a trigger can
`MODIFY` a nested composite field, and a function can return one built from
plain PHP arrays).
| NULL                           | unset / null         | `return;` or `null`     |

Arrays map naturally, including multidimensional arrays:

```sql
CREATE FUNCTION php_an_array() RETURNS int[] LANGUAGE plphp AS $$
    return array(array(1, 3, 5), array(2, 4, 6));
$$;
SELECT php_an_array();   -- {{1,3,5},{2,4,6}}
```

### Native jsonb via the `jsonb_plphp` transform

By default a `jsonb` value crosses the boundary as its text form. Install the
companion extension and declare `TRANSFORM FOR TYPE jsonb` to work with
**native PHP values** instead: JSON objects/arrays become PHP arrays, numbers
become int/float, booleans and null map directly, in both directions:

```sql
CREATE EXTENSION jsonb_plphp CASCADE;   -- pulls in plphp

CREATE FUNCTION redact(doc jsonb, key text) RETURNS jsonb
LANGUAGE plphp TRANSFORM FOR TYPE jsonb AS $$
    $walk = function (&$node) use (&$walk, $args) {
        foreach ($node as $k => &$v) {
            if ((string) $k === $args[1]) $v = '[redacted]';
            elseif (is_array($v))         $walk($v);
        }
    };
    $walk($args[0]);
    return $args[0];
$$;
```

Notes (shared with `jsonb_plperl`): returning PHP `null` yields SQL `NULL`,
not jsonb `null`; an empty PHP array comes back as `[]` (PHP cannot
distinguish an empty list from an empty map, the same ambiguity
`json_encode` has); JSON numbers arrive as PHP int when they fit, else float
(precision beyond a double is lost).

## Composite types and records

A composite argument arrives as an associative array keyed by column name; a
composite/record return value is built the same way.

```sql
CREATE TYPE tuple AS (name text, value integer);

CREATE FUNCTION make_tuple(text, integer) RETURNS tuple LANGUAGE plphp AS $$
    return array('name' => $args[0], 'value' => $args[1]);
$$;

SELECT * FROM make_tuple('answer', 42);   -- answer | 42
```

Functions declared `RETURNS record` must be called with a column definition
list, e.g. `SELECT * FROM f() AS (a int, b text)`.

Nesting converts recursively in both directions: a composite containing
arrays or other composites arrives as nested PHP arrays, and can be returned
the same way.

## Arguments

PL/php supports the full range of argument modes.

- **IN** (the default) arguments appear in `$args`.
- **OUT** / **INOUT** arguments: assign the result to a PHP variable named after
  the argument, or return an array of the OUT values.
- **TABLE(...)** columns behave like OUT arguments for a set-returning function.
- **VARIADIC** parameters arrive as a single PHP array holding the collected
  arguments (`VARIADIC "any"` is not supported).
- **Named** parameters are also available as `$name` (aliased to the matching
  `$args` element), in addition to positional `$args`.

```sql
CREATE FUNCTION add_sub(a integer, b integer, OUT sum integer, OUT diff integer)
LANGUAGE plphp AS $$
    $sum  = $a + $b;
    $diff = $a - $b;
$$;

SELECT * FROM add_sub(10, 4);   -- sum=14, diff=6
```

Named arguments must be valid PHP identifiers.

The same convention works for **INOUT parameters in procedures**: assign to
the variable; `CALL` reports the resulting values (always as a row, even for
a single INOUT parameter):

```sql
CREATE PROCEDURE bump(INOUT counter integer) LANGUAGE plphp AS $$
    $counter = $counter + 1;
$$;

CALL bump(41);   -- counter=42
```

## Set-returning functions

Declare the function `RETURNS SETOF ...` (or `RETURNS TABLE(...)`) and call
`return_next()` once per output row.

```sql
CREATE FUNCTION squares(lim integer)
RETURNS TABLE(n integer, square integer) LANGUAGE plphp AS $$
    for ($n = 1; $n <= $lim; $n++) {
        $square = $n * $n;
        return_next();               -- emit current $n, $square
    }
$$;

SELECT * FROM squares(3);            -- (1,1), (2,4), (3,9)
```

`return_next($value)` emits an explicit row (scalar, array, or associative
array matching the result columns). `return_next()` with no argument emits a row
built from the current OUT/TABLE variables.

Alternatively, return the whole result set at once as an array with one
element per row, like PL/Perl's "return a reference to an array" form. Use it
when the rows are already collected in a variable:

```sql
CREATE FUNCTION firstnames() RETURNS SETOF text LANGUAGE plphp AS $$
    $r = spi_exec("select fullname from people");
    $out = array();
    while ($row = spi_fetch_row($r))
        $out[] = explode(' ', $row['fullname'])[0];
    return $out;
$$;
```

If a function both calls `return_next` and returns an array, the array's rows
are appended after the `return_next` ones.

## Trigger functions

A trigger function is declared `RETURNS trigger`. Trigger metadata and the rows
involved are available in the associative array `$_TD`:

| Key                | Meaning                                             |
|--------------------|-----------------------------------------------------|
| `$_TD['name']`     | trigger name                                        |
| `$_TD['relid']`    | table OID                                           |
| `$_TD['relname']`  | table name                                          |
| `$_TD['schemaname']` | schema name                                       |
| `$_TD['when']`     | `BEFORE` or `AFTER`                                 |
| `$_TD['level']`    | `ROW` or `STATEMENT`                                |
| `$_TD['event']`    | `INSERT`, `UPDATE`, or `DELETE`                     |
| `$_TD['new']`      | new row (INSERT/UPDATE), as an associative array    |
| `$_TD['old']`      | old row (UPDATE/DELETE), as an associative array    |
| `$_TD['argc']`     | number of trigger arguments                         |
| `$_TD['args']`     | trigger arguments                                   |

Return value of a **BEFORE ... FOR EACH ROW** trigger:

- `return;` (NULL): proceed with the operation using the unmodified row.
- `return 'SKIP';`: silently skip the operation for this row.
- `return 'MODIFY';`: proceed using the (modified) `$_TD['new']` row. Modify
  fields in place, e.g. `$_TD['new']['col'] = 'value';`.

```sql
CREATE FUNCTION uppercase_name() RETURNS trigger LANGUAGE plphp AS $$
    $_TD['new']['name'] = strtoupper($_TD['new']['name']);
    return 'MODIFY';
$$;
```

## Event trigger functions

An event trigger function is declared `RETURNS event_trigger` and fires on DDL
events rather than on table rows. Its `$_TD` array carries:

| Key             | Meaning                                             |
|-----------------|-----------------------------------------------------|
| `$_TD['event']` | the firing event, e.g. `ddl_command_start`          |
| `$_TD['tag']`   | the command tag, e.g. `CREATE TABLE`                |

```sql
CREATE FUNCTION no_drop() RETURNS event_trigger LANGUAGE plphp AS $$
    if ($_TD['tag'] == 'DROP TABLE')
        pg_raise('error', 'dropping tables is not allowed');
$$;

CREATE EVENT TRIGGER guard ON ddl_command_start EXECUTE FUNCTION no_drop();
```

The return value of an event trigger function is ignored.

## Database access (SPI)

Run queries against the current database from within a function:

- `spi_exec(query [, limit])`: execute `query` (optionally limiting rows) and
  return a result resource. The call runs in a subtransaction that is rolled
  back automatically if the query raises an error.
- `spi_fetch_row(result)`: return the next row as an associative array, or
  `false` when the rows are exhausted.
- `spi_processed(result)`: number of rows the query produced.
- `spi_status(result)`: the SPI status code as a string.
- `spi_rewind(result)`: restart iteration from the first row.

```sql
CREATE FUNCTION sum_series(n integer) RETURNS integer LANGUAGE plphp AS $$
    $res = spi_exec("select generate_series(1, {$args[0]}) as g");
    $total = 0;
    while ($row = spi_fetch_row($res))
        $total += $row['g'];
    return $total;
$$;
```

### Streaming large result sets (cursors)

`spi_exec` materializes the whole result set in memory before you see the
first row. For result sets too large for that, open a cursor instead and
fetch rows one at a time:

- `spi_query(query)`: open a cursor for `query` and return its name (a
  string).
- `spi_fetchrow(cursor)`: return the next row as an associative array, or
  `false` when the cursor is exhausted (the cursor is then closed
  automatically).
- `spi_cursor_close(cursor)`: close a cursor early, before exhausting it.
  Closing an unknown or already-closed cursor is harmless.
- `spi_each(query, callable)`: run `query` and invoke `callable($row)` once
  per row, streaming over a cursor; return `false` from the callable to stop
  early. Returns the number of rows processed. The inline-loop equivalent of
  PL/Tcl's `spi_exec -array a $query { body }`.

Cursors not fetched to completion or closed explicitly are destroyed at the
end of the transaction.

```sql
CREATE FUNCTION first_negative() RETURNS integer LANGUAGE plphp AS $$
    $cur = spi_query("select v from readings order by taken_at");
    while ($row = spi_fetchrow($cur)) {
        if ($row['v'] < 0) {
            spi_cursor_close($cur);   -- stop early; skip the rest
            return $row['v'];
        }
    }
    return null;
$$;
```

## Prepared statements

For queries you run repeatedly, prepare a plan once and execute it with
parameters. `spi_prepare` takes the query text followed by the SQL type name of
each `$1`, `$2`, ... placeholder and returns a plan resource:

- `spi_prepare(query, type1, type2, ...)`: returns a plan.
- `spi_exec_prepared(plan, arg1, arg2, ...)`: execute the plan; returns a
  result resource just like `spi_exec` (use `spi_fetch_row`, `spi_processed`,
  etc.).
- `spi_query_prepared(plan, arg1, arg2, ...)`: open a *cursor* for the plan
  and return its name; fetch rows with `spi_fetchrow` (see
  [Streaming large result sets](#streaming-large-result-sets-cursors)).
  Before PL/php 2.1 this was an alias of `spi_exec_prepared`.
- `spi_freeplan(plan)`: release the plan when you are done with it.

```sql
CREATE FUNCTION lookup(int) RETURNS text LANGUAGE plphp AS $$
    $plan = spi_prepare('select name from things where id = $1', 'int4');
    $res  = spi_exec_prepared($plan, $args[0]);
    $row  = spi_fetch_row($res);
    spi_freeplan($plan);
    return $row['name'];
$$;
```

A plan can be cached in `$_SHARED` and reused across calls within a session for
better performance; free it with `spi_freeplan` when no longer needed.

## Transaction control

Inside a **procedure** invoked by `CALL` in a non-atomic context, you can commit
or roll back the current transaction:

- `spi_commit()`: commit the current transaction and begin a new one.
- `spi_rollback()`: roll back the current transaction and begin a new one.

```sql
CREATE PROCEDURE import_batch() LANGUAGE plphp AS $$
    for ($i = 0; $i < 1000; $i++) {
        spi_exec("insert into staging select * from source where batch = $i");
        spi_commit();            -- make each batch durable as it completes
    }
$$;

CALL import_batch();
```

These are only valid in a non-atomic call context. Calling them from an ordinary
function, or from a procedure invoked inside an explicit `BEGIN`/`COMMIT` block,
raises `invalid transaction termination`.

## Subtransactions

`subtransaction(callable [, arg, ...])` runs a PHP callable inside an internal
subtransaction. Any extra arguments are passed through to the callable, and its
return value is returned. If the callable **throws a PHP exception**, the
subtransaction's database changes are rolled back and the exception propagates
to the caller, where it can be caught:

```sql
CREATE FUNCTION safe_insert(text) RETURNS text LANGUAGE plphp AS $$
    try {
        subtransaction(function($v) {
            spi_exec("insert into t(name) values (" . quote_literal($v) . ")");
            if ($v === '') throw new Exception('empty name');
        }, $args[0]);
        return 'inserted';
    } catch (\Throwable $e) {
        return 'skipped: ' . $e->getMessage();   // the insert was rolled back
    }
$$;
```

A *database* error inside the block (for example a constraint violation
surfaced by `spi_exec`) likewise rolls the subtransaction back and propagates
as a catchable [`PgError`](#errors-and-exceptions). Note that each SPI call
already runs in its own subtransaction, so `try`/`catch` around a single call
does not need `subtransaction()`; use it to make a *group* of statements
succeed or fail atomically.

## Quoting helpers

When building SQL dynamically, quote values and identifiers so the result is
safe and syntactically correct:

- `quote_literal(string)`: quote a value as an SQL string literal.
- `quote_nullable(value)`: like `quote_literal`, but a PHP `null` becomes the
  SQL keyword `NULL`.
- `quote_ident(name)`: quote a string for use as an SQL identifier (only when
  needed).

```php
$sql = "select * from " . quote_ident($table) .
       " where name = " . quote_literal($name);
```

## Messaging: elog and pg_raise

`elog(level, message)` emits a message at any PostgreSQL log level:

```php
elog('DEBUG',   'detailed diagnostics');
elog('LOG',     'goes to the server log');
elog('INFO',    'informational');
elog('NOTICE',  'shown to the client');
elog('WARNING', 'something looks off');
elog('ERROR',   'stop right here');   // aborts, like a PostgreSQL ERROR
```

The level is one of `DEBUG`, `LOG`, `INFO`, `NOTICE`, `WARNING`, or `ERROR`
(case-insensitive). `pg_raise(level, message)` is an older, narrower spelling
that accepts `notice`, `warning`, or `error`. Anything PHP writes to standard
output is also forwarded to the PostgreSQL log.

## Shared data: `$_SHARED`

`$_SHARED` is an associative array that persists across function calls within
the same database session. Use it to cache data or share state between PL/php
functions:

```sql
CREATE FUNCTION set_shared(key text, val text) RETURNS void LANGUAGE plphp AS $$
    $_SHARED[$args[0]] = $args[1];
$$;

CREATE FUNCTION get_shared(key text) RETURNS text LANGUAGE plphp AS $$
    return $_SHARED[$args[0]];
$$;
```

## Session initialization: modules and start_proc

Two mechanisms let you run PHP setup code the first time PL/php is used in a
session.

**Modules.** If a table named `plphp_modules(modname text, modseq int, modsrc
text)` exists, its rows are loaded (ordered by `modname`, `modseq`) when the
interpreter initializes. Any functions or classes the code defines become
available to every PL/php function in the session, a place for a
shared library of helpers:

```sql
CREATE TABLE plphp_modules (modname text, modseq int, modsrc text);
INSERT INTO plphp_modules VALUES
    ('util', 0, 'function slugify($s) { return strtolower(trim($s)); }');
-- slugify() is now callable from any PL/php function in new sessions.
```

**start_proc.** The `plphp.start_proc` configuration setting names a PL/php
function to call once, when the interpreter is first initialized in a session:

```sql
-- e.g. in postgresql.conf, ALTER DATABASE ... SET, or a session that has
-- already loaded PL/php:
SET plphp.start_proc = 'my_setup';
```

**on_init.** The `plphp.on_init` setting holds a snippet of *PHP source* to
execute at initialization, the counterpart of `plperl.on_init`. Use it for
setup that doesn't warrant a modules table, such as defining a helper or
setting an include path:

```sql
SET plphp.on_init = 'function app_env() { return "production"; }';
```

All three run inside the first PL/php call of the session, in this order:
`on_init`, then modules, then `start_proc`.

## Errors and exceptions

Function bodies are syntax-checked at `CREATE FUNCTION` time by the validator;
an invalid body is rejected with the PHP parse error.

**Database errors are catchable.** Every database error raised by an SPI call
(`spi_exec`, `spi_fetchrow`, `spi_commit`, ...) is thrown as a **`PgError`**
exception, the counterpart of PL/Perl's `eval`-trappable errors and PL/Tcl's
`catch`. The failed call's subtransaction has already been rolled back when
you catch it, so the session is in a consistent state and the function can
continue:

```sql
CREATE FUNCTION upsertish(int) RETURNS text LANGUAGE plphp AS $$
    try {
        spi_exec("insert into t values ({$args[0]})");
        return 'inserted';
    } catch (PgError $e) {
        if ($e->getSQLState() == '23505') {    -- unique_violation
            spi_exec("update t set n = n + 1 where id = {$args[0]}");
            return 'bumped';
        }
        throw $e;                              -- anything else: re-raise
    }
$$;
```

`PgError extends Exception` and adds:

- `getSQLState()`: the five-character SQLSTATE code (e.g. `23505`).
- `getDetail()` / `getHint()`: the error's DETAIL and HINT, or `null`.

`pg_raise('error', ...)` and `elog('ERROR', ...)` also throw a `PgError`
(SQLSTATE `P0001`, like PL/pgSQL's `RAISE`). An **uncaught** `PgError`, or
any other uncaught PHP exception such as a `TypeError`, is reported as a
PostgreSQL `ERROR`, aborting the statement. PHP deprecation notices (such as
the legacy `"${var}"` string interpolation) are surfaced as PostgreSQL
`NOTICE`s and are not fatal.

## Security

PL/php is an **untrusted** language. Historically the trusted variant restricted
user code using PHP's `safe_mode`, but **`safe_mode` was removed in PHP 5.4**, so
on modern PHP nothing sandboxes a PL/php function: it can do whatever the
PostgreSQL server's operating-system user can do: read and write files, open
network connections, run shell commands, and so on.

Accordingly, the language is created without the `TRUSTED` attribute: the
extension is superuser-only to install, and only superusers can create PL/php
functions. Do not hand that ability to roles you would not trust with the
server's operating-system account.
