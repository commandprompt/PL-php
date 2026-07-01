# PL/php Language Reference

PL/php lets you write PostgreSQL functions and triggers in PHP. This document
describes the programming interface. For build and install instructions see
[`INSTALL`](../INSTALL); for a feature summary see [`README`](../README).

Tested with **PostgreSQL 18** and **PHP 8.3** (embed SAPI, non-thread-safe);
also builds on PostgreSQL 16.

- [Enabling the language](#enabling-the-language)
- [Writing functions](#writing-functions)
- [Data type mapping](#data-type-mapping)
- [Composite types and records](#composite-types-and-records)
- [Arguments: IN, OUT, INOUT, TABLE, named](#arguments)
- [Set-returning functions](#set-returning-functions)
- [Trigger functions](#trigger-functions)
- [Database access (SPI)](#database-access-spi)
- [Messaging: pg_raise](#messaging-pg_raise)
- [Shared data: `$_SHARED`](#shared-data-_shared)
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

- `$args` — a 0-indexed array of the call arguments.
- `$argc` — the number of declared arguments.
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
| NULL                           | unset / null         | `return;` or `null`     |

Arrays map naturally, including multidimensional arrays:

```sql
CREATE FUNCTION php_an_array() RETURNS int[] LANGUAGE plphp AS $$
    return array(array(1, 3, 5), array(2, 4, 6));
$$;
SELECT php_an_array();   -- {{1,3,5},{2,4,6}}
```

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

## Arguments

PL/php supports the full range of argument modes.

- **IN** (the default) arguments appear in `$args`.
- **OUT** / **INOUT** arguments: assign the result to a PHP variable named after
  the argument, or return an array of the OUT values.
- **TABLE(...)** columns behave like OUT arguments for a set-returning function.
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

- `return;` (NULL) — proceed with the operation using the unmodified row.
- `return 'SKIP';` — silently skip the operation for this row.
- `return 'MODIFY';` — proceed using the (modified) `$_TD['new']` row. Modify
  fields in place, e.g. `$_TD['new']['col'] = 'value';`.

```sql
CREATE FUNCTION uppercase_name() RETURNS trigger LANGUAGE plphp AS $$
    $_TD['new']['name'] = strtoupper($_TD['new']['name']);
    return 'MODIFY';
$$;
```

## Database access (SPI)

Run queries against the current database from within a function:

- `spi_exec(query [, limit])` — execute `query` (optionally limiting rows) and
  return a result resource. The call runs in a subtransaction that is rolled
  back automatically if the query raises an error.
- `spi_fetch_row(result)` — return the next row as an associative array, or
  `false` when the rows are exhausted.
- `spi_processed(result)` — number of rows the query produced.
- `spi_status(result)` — the SPI status code as a string.
- `spi_rewind(result)` — restart iteration from the first row.

```sql
CREATE FUNCTION sum_series(n integer) RETURNS integer LANGUAGE plphp AS $$
    $res = spi_exec("select generate_series(1, {$args[0]}) as g");
    $total = 0;
    while ($row = spi_fetch_row($res))
        $total += $row['g'];
    return $total;
$$;
```

## Messaging: pg_raise

Emit a message to the client/log at a chosen level:

```php
pg_raise('notice',  'just so you know');
pg_raise('warning', 'something looks off');
pg_raise('error',   'stop right here');   -- aborts, like a PostgreSQL ERROR
```

The level is one of `notice`, `warning`, or `error` (case-insensitive). Anything
PHP writes to standard output is also forwarded to the PostgreSQL log.

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

## Errors and exceptions

Function bodies are syntax-checked at `CREATE FUNCTION` time by the validator;
an invalid body is rejected with the PHP parse error.

At run time, an uncaught PHP exception (for example, calling an undefined
function or a `TypeError`) is reported as a PostgreSQL `ERROR`, aborting the
statement. PHP deprecation notices (such as the legacy `"${var}"` string
interpolation) are surfaced as PostgreSQL `NOTICE`s and are not fatal. You can
raise an error yourself with `pg_raise('error', ...)`.

## Security

PL/php is an **untrusted** language. Historically the trusted variant restricted
user code using PHP's `safe_mode`, but **`safe_mode` was removed in PHP 5.4**, so
on modern PHP nothing sandboxes a PL/php function: it can do whatever the
PostgreSQL server's operating-system user can do — read and write files, open
network connections, run shell commands, and so on.

Accordingly, the language is created without the `TRUSTED` attribute: the
extension is superuser-only to install, and only superusers can create PL/php
functions. Do not hand that ability to roles you would not trust with the
server's operating-system account.
