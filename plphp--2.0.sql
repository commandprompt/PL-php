/* PL/php language, packaged as an extension.
 *
 * The language is UNTRUSTED (created without the TRUSTED attribute): on modern
 * PHP there is no sandbox, so PL/php functions can do anything the server's OS
 * user can.  Creating them is therefore restricted to superusers.
 */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plphp" to load this file. \quit

CREATE FUNCTION plphp_call_handler()
RETURNS language_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION plphp_validator(oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE LANGUAGE plphp
    HANDLER plphp_call_handler
    VALIDATOR plphp_validator;

COMMENT ON LANGUAGE plphp IS 'PL/php procedural language (untrusted)';
