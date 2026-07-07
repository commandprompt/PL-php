/**********************************************************************
 * bytea_plphp.c
 *
 * Transform between bytea and PL/php: with CREATE FUNCTION ... TRANSFORM
 * FOR TYPE bytea, a bytea argument arrives in PHP as a raw binary string
 * (the bytes themselves, not the "\x..." text form), and a PHP string
 * returned from the function is stored verbatim as bytea.  This mirrors
 * PL/Python's bytea <-> bytes mapping.
 *
 * PHP strings are length-counted and binary-safe, so embedded NUL bytes
 * survive the round trip in both directions (unlike the default text path,
 * which routes bytea through byteaout/byteain).
 *
 * This software is copyright (c) Command Prompt Inc.  Same license as
 * PL/php itself; see plphp.c.
 *********************************************************************
 */

#include "postgres.h"

#include "fmgr.h"
/* The varlena macros (VARDATA/SET_VARSIZE/...) moved to varatt.h in PG 16;
 * before that they live in postgres.h, which is always included. */
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
#include "utils/builtins.h"

/*
 * These are defined again in php.h, so undef them to avoid some
 * cpp warnings.
 */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#include "php.h"
#include "Zend/zend_API.h"

PG_MODULE_MAGIC;

/*
 * bytea -> PHP (the FROM SQL transform function)
 *
 * The bytes become a binary-safe PHP string.  PG_GETARG_BYTEA_PP detoasts
 * a compressed or out-of-line value first.
 */
PG_FUNCTION_INFO_V1(bytea_to_plphp);

Datum
bytea_to_plphp(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	zval	   *z = (zval *) emalloc(sizeof(zval));

	ZVAL_STRINGL(z, VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in));

	PG_RETURN_POINTER(z);
}

/*
 * PHP -> bytea (the TO SQL transform function)
 *
 * A PHP string is stored byte-for-byte.  Any other type is rejected: bytea
 * has no meaningful mapping from an array/object, and silently coercing an
 * int/float/bool to its textual bytes would surprise more than it helps.
 */
PG_FUNCTION_INFO_V1(plphp_to_bytea);

Datum
plphp_to_bytea(PG_FUNCTION_ARGS)
{
	zval	   *in = (zval *) PG_GETARG_POINTER(0);
	size_t		len;
	bytea	   *out;

	ZVAL_DEREF(in);

	if (Z_TYPE_P(in) != IS_STRING)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot transform this PHP value to bytea"),
				 errdetail("Only a PHP string maps to bytea; got PHP type %d.",
						   (int) Z_TYPE_P(in))));

	len = Z_STRLEN_P(in);
	out = (bytea *) palloc(len + VARHDRSZ);
	SET_VARSIZE(out, len + VARHDRSZ);
	memcpy(VARDATA(out), Z_STRVAL_P(in), len);

	PG_RETURN_BYTEA_P(out);
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
