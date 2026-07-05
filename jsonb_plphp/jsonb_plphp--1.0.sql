/* jsonb_plphp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_plphp" to load this file. \quit

CREATE FUNCTION jsonb_to_plphp(internal)
RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION plphp_to_jsonb(internal)
RETURNS jsonb
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR jsonb LANGUAGE plphp (
    FROM SQL WITH FUNCTION jsonb_to_plphp(internal),
    TO SQL WITH FUNCTION plphp_to_jsonb(internal));

COMMENT ON TRANSFORM FOR jsonb LANGUAGE plphp IS
    'transform between jsonb and PL/php';
