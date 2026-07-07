/* bytea_plphp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION bytea_plphp" to load this file. \quit

CREATE FUNCTION bytea_to_plphp(internal)
RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plphp_to_bytea(internal)
RETURNS bytea
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR bytea LANGUAGE plphp (
    FROM SQL WITH FUNCTION bytea_to_plphp(internal),
    TO SQL WITH FUNCTION plphp_to_bytea(internal));

COMMENT ON TRANSFORM FOR bytea LANGUAGE plphp IS
    'transform between bytea and PL/php';
