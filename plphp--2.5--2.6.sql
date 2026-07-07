/* Upgrade PL/php from 2.5 to 2.6.
 *
 * 2.6 adds $_SD, a per-function static dictionary that persists across calls
 * within a session (the private counterpart to the session-global $_SHARED).
 * The companion bytea_plphp transform ships as its own extension.  All of it
 * lives in the C module, so there is nothing to change at the SQL level.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
