# PL/php vs PL/Tcl feature comparison

This compares PL/php against PostgreSQL's built-in PL/Tcl. Items marked
**added** were implemented to match PL/Tcl; the one remaining gap is noted with
its rationale.

## Language capabilities

| Capability                              | PL/Tcl | PL/php | Notes |
|-----------------------------------------|:------:|:------:|-------|
| Scalar / composite / array functions    | ✅ | ✅ | |
| Set-returning functions                 | ✅ | ✅ | `return_next` |
| DML trigger functions                   | ✅ | ✅ | `$_TD` in PL/php; `$TG_*`/`$NEW`/`$OLD` in PL/Tcl |
| **Event trigger functions**             | ✅ | ✅ **added** | `RETURNS event_trigger`; `$_TD['event']`, `$_TD['tag']` |
| Session-shared data                     | `GD` array | `$_SHARED` | |
| **Private per-function data**           | ❌ | `$_SD` **added** | like PL/Python's `SD` (`$_SHARED` is the `GD` counterpart) |
| Transaction control in procedures       | `commit`/`rollback` | `spi_commit`/`spi_rollback` | |
| **Explicit subtransactions**            | `subtransaction {…}` | `subtransaction(callable, …)` **added** | |
| **Module autoloading**                  | `pltcl_modules` + `unknown` | `plphp_modules` **added** | PL/php eager-loads all module rows at interpreter init |
| **Start proc**                          | `pltcl.start_proc` | `plphp.start_proc` **added** | |
| Trusted (sandboxed) variant             | `pltcl` (Safe-Tcl) | ❌ by design | see below |

## Built-in commands / functions

| PL/Tcl                        | PL/php | Notes |
|-------------------------------|--------|-------|
| `spi_exec ?-count n? cmd`     | `spi_exec(cmd [, limit])` | |
| `spi_prepare` / `spi_execp`   | `spi_prepare` / `spi_exec_prepared` | |
| row iteration (`-array`/loop) | `spi_each(query, callable)` **added** | Streaming per-row callback, the inline-loop-body equivalent |
| streaming row-by-row (portal) | `spi_query` / `spi_fetchrow` / `spi_cursor_close` **added** | Constant-memory iteration over large result sets |
| `catch` with rich `errorCode` | `try`/`catch (PgError $e)` **added** | SQLSTATE via `getSQLState()`, plus detail and hint |
| `quote`                       | `quote_literal` (+ `quote_nullable`, `quote_ident`) | |
| `elog level msg`              | `elog(level, msg)` | DEBUG/LOG/INFO/NOTICE/WARNING/ERROR |
| `subtransaction { body }`     | `subtransaction(callable, …)` **added** | |
| `commit` / `rollback`         | `spi_commit` / `spi_rollback` | |
| (no transform support)        | `jsonb_plphp` **added** | `TRANSFORM FOR TYPE jsonb`; PL/Tcl has no transforms at all |

## The one remaining gap

- **Trusted / sandboxed language.** PL/Tcl's `pltcl` runs untrusted user code in
  a Safe-Tcl interpreter that restricts filesystem, network, and process access,
  so it is safe for non-superusers. Modern PHP has no comparable sandbox
  (`safe_mode` was removed in PHP 5.4), so PL/php is untrusted and superuser-only
  (see [Security](plphp.md#security)). This is the only PL/Tcl capability PL/php
  cannot practically match.

## Notes on the additions

- **Event triggers** dispatch through the same call handler; the function is
  declared `RETURNS event_trigger` and receives `$_TD` with `event` and `tag`.
- **Subtransactions** roll back on a thrown PHP exception (which the caller can
  catch) or on a database error (which, under PL/php's error model, aborts the
  statement). See [Subtransactions](plphp.md#subtransactions).
- **Modules** are loaded by wrapping each row's source in a function and running
  it, so nested `function`/`class` declarations register globally for the
  session.
- **start_proc** runs during the first PL/php call of a session.

See [`doc/plphp.md`](plphp.md) for full usage, and
[`doc/plperl-comparison.md`](plperl-comparison.md) for the PL/Perl comparison.
