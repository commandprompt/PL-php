/* Upgrade PL/php from 2.2 to 2.3.
 *
 * 2.3 adds VARIADIC parameter support, converts array columns inside rows
 * to PHP arrays, adds CI, supports PHP 8.1 through 8.4, and hardens the
 * interpreter against host php.ini settings.  All of it lives in the C
 * module, which PostgreSQL loads by path, so there is nothing to change at
 * the SQL level.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
