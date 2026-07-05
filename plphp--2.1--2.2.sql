/* Upgrade PL/php from 2.1 to 2.2.
 *
 * 2.2 adds catchable PgError exceptions, whole-array set-returning
 * functions, spi_each, plphp.on_init, TRANSFORM FOR TYPE support, the
 * array-conversion rewrite, and the INOUT-procedures fix.  All of it lives
 * in the C module, which PostgreSQL loads by path, so there is nothing to
 * change at the SQL level.  (The jsonb transform is the separate
 * jsonb_plphp extension.)
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
