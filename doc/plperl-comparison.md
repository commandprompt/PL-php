# PL/php vs PL/Perl feature comparison

This compares PL/php against PostgreSQL's built-in PL/Perl and records which
PL/Perl features PL/php now provides. Items marked **added** were implemented so
that PL/php reaches parity with PL/Perl's function set; a few items are
intentionally out of scope, with the rationale given.

## Language capabilities

| Capability                              | PL/Perl | PL/php | Notes |
|-----------------------------------------|:-------:|:------:|-------|
| Scalar functions                        | ✅ | ✅ | |
| Array arguments / return (multi-dim)    | ✅ | ✅ | |
| Composite / record arguments and return | ✅ | ✅ | |
| Set-returning functions (`return_next`) | ✅ | ✅ | |
| `RETURNS TABLE` / OUT / INOUT arguments  | ✅ | ✅ | PL/php also supports named parameters, which PL/Perl does not |
| Trigger functions (`$_TD`)              | ✅ | ✅ | |
| Session-shared data                     | `%_SHARED` | `$_SHARED` | |
| **Anonymous code blocks (`DO`)**        | ✅ | ✅ **added** | `DO $$ ... $$ LANGUAGE plphp` |
| **Procedures with transaction control** | ✅ | ✅ **added** | `CALL` a `PROCEDURE`; `spi_commit`/`spi_rollback` in a non-atomic context |
| Trusted (sandboxed) variant             | `plperl` (Safe.pm) | ❌ by design | PHP's `safe_mode` was removed in 5.4; PL/php is untrusted/superuser-only (see [Security](plphp.md#security)) |

## Built-in functions

| PL/Perl                       | PL/php | Notes |
|-------------------------------|--------|-------|
| `spi_exec_query`              | `spi_exec` | Runs a query, returns a result to iterate |
| `spi_fetchrow` / row access   | `spi_fetch_row` | Iterates a materialized `spi_exec` result |
| (row count / status)          | `spi_processed`, `spi_status`, `spi_rewind` | |
| `spi_query`                   | `spi_query` **added** | Opens a cursor; streams rows without materializing |
| `spi_fetchrow`                | `spi_fetchrow` **added** | Fetch next row from a cursor; auto-closes at exhaustion |
| `spi_cursor_close`            | `spi_cursor_close` **added** | Abandon a cursor early |
| errors trappable with `eval`  | `try`/`catch (PgError $e)` **added** | PgError carries SQLSTATE, detail, and hint (richer than `$@`) |
| SRF returns array reference   | return an array from the SRF **added** | One element per row, as an alternative to `return_next` |
| —                             | `spi_each(query, callable)` **added** | Streaming per-row callback; PL/Perl has no equivalent |
| `elog(level, msg)`            | `elog` **added** | DEBUG/LOG/INFO/NOTICE/WARNING/ERROR; `pg_raise` remains as the older spelling |
| `spi_prepare`                 | `spi_prepare` **added** | Type names given as SQL type strings |
| `spi_exec_prepared`           | `spi_exec_prepared` **added** | |
| `spi_query_prepared`          | `spi_query_prepared` **added** | Opens a cursor for a prepared plan (was an alias of `spi_exec_prepared` in 2.0) |
| `spi_freeplan`                | `spi_freeplan` **added** | |
| `quote_literal`               | `quote_literal` **added** | |
| `quote_nullable`              | `quote_nullable` **added** | |
| `quote_ident`                 | `quote_ident` **added** | |
| `spi_commit` / `spi_rollback` | `spi_commit` / `spi_rollback` **added** | Transaction control in a procedure invoked by `CALL` (non-atomic) |
| `looks_like_number`           | — | Use PHP's built-in `is_numeric()` |
| `encode_bytea` / `decode_bytea` | — | Use PHP's `bin2hex` / `hex2bin`, `base64_encode`, etc. |
| `encode_array_literal` / `encode_typed_literal` | — | Marginal; build literals with the quoting helpers |

## Interpreter configuration

| PL/Perl               | PL/php | Notes |
|-----------------------|--------|-------|
| `plperl.on_init`      | `plphp.on_init` **added** | PHP source run at interpreter initialization |
| (start proc, PL/Tcl-style) | `plphp.start_proc` | Runs a named PL/php function once per session |
| `plperl.use_strict`   | — | No PHP equivalent; PHP is always "strict" about undefined functions |

## Intentionally not implemented

- **Trusted/sandboxed language.** PL/Perl's `plperl` uses `Safe.pm` to restrict
  operations. Modern PHP has no equivalent (`safe_mode` was removed in PHP 5.4),
  so PL/php is untrusted only.

See [`doc/plphp.md`](plphp.md) for full usage of the functions listed above.
