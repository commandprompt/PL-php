# Changelog

All notable changes to PL/php are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project aims to follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Cursor-streaming SPI** — `spi_query(query)` opens a cursor and returns its
  name, `spi_fetchrow(cursor)` fetches one row at a time (returning `false`
  and closing the cursor at exhaustion), and `spi_cursor_close(cursor)`
  abandons a cursor early. Large result sets can now be scanned in constant
  memory instead of being materialized by `spi_exec`. Matches PL/Perl's
  `spi_query`/`spi_fetchrow`/`spi_cursor_close`.

### Changed

- **Breaking:** `spi_query_prepared(plan, args...)` now opens a cursor and
  returns its name for use with `spi_fetchrow`, matching PL/Perl semantics.
  In 2.0 it was an alias of `spi_exec_prepared` (returning a materialized
  result resource); code that relied on the alias should call
  `spi_exec_prepared` instead.

## [2.0.0] — 2026-07-01

A ground-up modernization of PL/php (the previous release, 1.4, dates from 2010)
for current software. Tested on **PostgreSQL 11 through 18** with **PHP 8.3**
(embed SAPI, non-thread-safe).

### Added

- **Anonymous `DO` blocks** — `DO $$ ... $$ LANGUAGE plphp`, via an inline
  handler.
- **Event trigger functions** — `RETURNS event_trigger`, with `$_TD['event']`
  and `$_TD['tag']`.
- **Prepared statements** — `spi_prepare`, `spi_exec_prepared`,
  `spi_query_prepared`, and `spi_freeplan`.
- **Transaction control** in procedures — `spi_commit` and `spi_rollback`.
- **Explicit subtransactions** — `subtransaction(callable, ...)`.
- **Quoting helpers** — `quote_literal`, `quote_nullable`, `quote_ident`.
- **`elog(level, message)`** supporting `DEBUG`/`LOG`/`INFO`/`NOTICE`/`WARNING`/`ERROR`.
- **Session initialization** — module autoloading from a `plphp_modules` table
  and a `plphp.start_proc` configuration setting.
- Packaging as a first-class extension (`CREATE EXTENSION plphp`) and a
  regression test for every new feature.
- Documentation: a language reference (`doc/plphp.md`) and PL/Perl and PL/Tcl
  feature comparisons.

### Changed

- Ported the C code to the PostgreSQL 18 API: `FunctionCallInfo.args[]` (PG 12),
  `TupleDescAttr` (PG 10), `SearchSysCache1`, `strlcpy`, `TextDatumGetCString`,
  `ALLOCSET_DEFAULT_SIZES`, `uint64` SPI row counts (PG 11), and the removal of
  `SPI_restore_connection` (PG 10).
- Ported the interpreter glue to the PHP 8 Zend API: the new zval/refcount
  model, hash-table and resource APIs, `call_user_function`,
  `zend_rebuild_symbol_table`, embed startup, and the PHP 8.1+ `zend_error_cb`
  signature.
- Uncaught PHP exceptions (for example calling an undefined function) are now
  reported as PostgreSQL errors instead of silently returning `NULL`.
- PHP deprecations (such as the legacy `"${var}"` string interpolation) are now
  non-fatal notices.
- Replaced the autoconf build with a PGXS `Makefile`.
- Version guards keep releases back to PostgreSQL 11 working (a
  `FunctionCallInfo` argument shim before PG 12, the pre-`CommandTag`
  event-trigger tag before PG 13, `EmitWarningsOnPlaceholders` before PG 15, and
  an explicit `SPI_start_transaction` after commit/rollback before PG 15).

### Removed

- The obsolete `pg_pltemplate` install scripts (the catalog was removed in
  PostgreSQL 13).
- The redundant `plphpu` variant.

### Security

- PHP's `safe_mode` was removed in PHP 5.4, so PL/php can no longer be
  sandboxed. It is now an **untrusted, superuser-only** language, created without
  the `TRUSTED` attribute; only superusers may install the extension or create
  PL/php functions.

## [1.4] — 2010-07-12

### Added

- Support for PostgreSQL 8.4 and 9.0.
- Support for PHP 5.3.

## [1.3.5-beta1] — 2007-10-15

### Added

- Support for parameter names.
- Support for PostgreSQL 8.3.

## [1.3.3] — 2007-03-29

### Added

- `bool` type in return values.
- Column names resembling numbers.

### Changed

- Map PHP `E_STRICT` to `WARNING` instead of `ERROR`.

### Fixed

- Several memory leaks.
- Bugs in argument handling.

## [1.3.2] — 2007-03-01

### Changed

- Link against the PHP embed SAPI instead of Apache's `mod_php`, making the
  build far more robust against internal PHP changes.

### Added

- `configure` support for detecting required utilities and libraries.

## [1.3.1] — 2006-12-01

### Changed

- Minor Makefile cleanups.

## [1.2] — 2005-12-13

### Added

- Set-returning functions.
- Support for PostgreSQL 8.0.

## [1.1] — 2005-12-05

Supports PostgreSQL 8.0 and 8.1.

### Added

- A PGXS-based build that no longer requires the PHP or PostgreSQL sources.
- Rudimentary SPI support for running queries and processing results.
- Trigger support, including aborting/skipping an operation and modifying the
  tuple before insert/update.
- Function validation at creation time (syntax errors are reported immediately).
- Propagation of PHP errors and warnings to PostgreSQL.

### Changed

- Overhauled memory handling; SPI results are now opaque PHP resources, and a
  private symbol table is created and cleared per call.
