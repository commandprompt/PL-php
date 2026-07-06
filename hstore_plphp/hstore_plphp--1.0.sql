/* hstore_plphp--1.0.sql
 *
 * Transform between hstore and PL/php arrays.
 *
 * With this transform, a function created with TRANSFORM FOR TYPE hstore
 * receives hstore arguments as a PHP associative array of string keys to
 * string (or null) values, and may return a PHP array into an hstore
 * result.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plphp" to load this file. \quit

CREATE FUNCTION hstore_to_plphp(internal)
RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plphp_to_hstore(internal)
RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE plphp (
    FROM SQL WITH FUNCTION hstore_to_plphp(internal),
    TO SQL WITH FUNCTION plphp_to_hstore(internal));

COMMENT ON TRANSFORM FOR hstore LANGUAGE plphp IS
    'transform between hstore and PL/php arrays';
