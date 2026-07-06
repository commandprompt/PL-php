/* Upgrade PL/php from 2.3 to 2.4.
 *
 * 2.4 adds per-function memory contexts, error CONTEXT lines, structural
 * composite conversion, the anycompatible polymorphics, a fix for a crash
 * in nested error propagation, an ASAN CI job, and a benchmark suite.  All
 * of it lives in the C module or the repository, so there is nothing to
 * change at the SQL level.
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION plphp UPDATE" to load this file. \quit
