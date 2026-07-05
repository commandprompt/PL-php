/* Upgrade PL/php from 2.0 to 2.1.
 *
 * 2.1 adds the cursor-streaming SPI functions (spi_query, spi_fetchrow,
 * spi_cursor_close) and changes spi_query_prepared to open a cursor.  All of
 * these live in the C module, which PostgreSQL loads by path, so there is
 * nothing to change at the SQL level.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
