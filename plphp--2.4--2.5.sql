/* Upgrade PL/php from 2.4 to 2.5.
 *
 * 2.5 adds structured pg_raise (DETAIL/HINT/SQLSTATE), TRUNCATE and INSTEAD OF
 * trigger support, domains over arrays and composites, and transforms that
 * reach nested contexts (trigger rows, composites, set-returning results).
 * The companion hstore_plphp transform ships as its own extension.  All of it
 * lives in the C module, so there is nothing to change at the SQL level.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
