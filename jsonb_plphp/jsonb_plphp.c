/**********************************************************************
 * jsonb_plphp.c
 *
 * Transform between jsonb and PL/php: with CREATE FUNCTION ... TRANSFORM
 * FOR TYPE jsonb, a jsonb argument arrives in PHP as a native value
 * (array / int / float / bool / string / null) instead of its text form,
 * and a PHP value returned from the function is converted back to jsonb
 * directly.
 *
 * This software is copyright (c) Command Prompt Inc.  Same license as
 * PL/php itself; see plphp.c.
 *********************************************************************
 */

#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

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

static zval *jsonb_container_to_zval(JsonbContainer *in);
static JsonbValue *zval_to_jsonbvalue(zval *val, JsonbParseState **ps,
									  bool is_elem);

/*
 * Convert a scalar JsonbValue (or a nested container, which the iterator
 * hands us as jbvBinary) to a freshly emalloc'd zval.
 */
static zval *
jsonb_scalar_to_zval(JsonbValue *v)
{
	zval	   *z;

	if (v->type == jbvBinary)
		return jsonb_container_to_zval((JsonbContainer *) v->val.binary.data);

	z = (zval *) emalloc(sizeof(zval));

	switch (v->type)
	{
		case jbvNull:
			ZVAL_NULL(z);
			break;
		case jbvBool:
			ZVAL_BOOL(z, v->val.boolean);
			break;
		case jbvString:
			ZVAL_STRINGL(z, v->val.string.val, v->val.string.len);
			break;
		case jbvNumeric:
			{
				char	   *num;
				char	   *end;
				long		lval;

				num = DatumGetCString(DirectFunctionCall1(numeric_out,
									  NumericGetDatum(v->val.numeric)));
				/* integral values that fit become PHP ints, else floats */
				errno = 0;
				lval = strtol(num, &end, 10);
				if (*end == '\0' && errno != ERANGE)
					ZVAL_LONG(z, (zend_long) lval);
				else
					ZVAL_DOUBLE(z, strtod(num, NULL));
				pfree(num);
				break;
			}
		default:
			elog(ERROR, "unexpected jsonb value type: %d", (int) v->type);
	}

	return z;
}

/*
 * Convert a JsonbContainer to a freshly emalloc'd zval: a list-style PHP
 * array for a JSON array, an associative array for a JSON object, or the
 * scalar itself for a raw-scalar pseudo array.
 */
static zval *
jsonb_container_to_zval(JsonbContainer *in)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	zval	   *z;

	it = JsonbIteratorInit(in);
	r = JsonbIteratorNext(&it, &v, true);

	if (r == WJB_BEGIN_ARRAY && v.val.array.rawScalar)
	{
		/* a bare scalar at the top level: unwrap it */
		r = JsonbIteratorNext(&it, &v, true);
		Assert(r == WJB_ELEM);
		z = jsonb_scalar_to_zval(&v);
		(void) JsonbIteratorNext(&it, &v, true);	/* WJB_END_ARRAY */
		return z;
	}

	z = (zval *) emalloc(sizeof(zval));
	array_init(z);

	if (r == WJB_BEGIN_ARRAY)
	{
		while ((r = JsonbIteratorNext(&it, &v, true)) == WJB_ELEM)
		{
			zval	   *el = jsonb_scalar_to_zval(&v);

			add_next_index_zval(z, el);
			efree(el);
		}
	}
	else if (r == WJB_BEGIN_OBJECT)
	{
		while ((r = JsonbIteratorNext(&it, &v, true)) == WJB_KEY)
		{
			char	   *key = pnstrdup(v.val.string.val, v.val.string.len);
			zval	   *el;

			r = JsonbIteratorNext(&it, &v, true);
			if (r != WJB_VALUE)
				elog(ERROR, "unexpected jsonb iterator token: %d", (int) r);

			el = jsonb_scalar_to_zval(&v);
			add_assoc_zval(z, key, el);
			efree(el);
			pfree(key);
		}
	}
	else
		elog(ERROR, "unexpected jsonb iterator token: %d", (int) r);

	return z;
}

/*
 * Is this PHP array a list (keys 0..n-1 in order), i.e. a JSON array
 * rather than a JSON object?
 */
static bool
plphp_array_is_list(HashTable *ht)
{
#if PHP_VERSION_ID >= 80100
	return zend_array_is_list(ht);
#else
	zend_ulong	expected = 0;
	zend_string *strkey;
	zend_ulong	numkey;

	ZEND_HASH_FOREACH_KEY(ht, numkey, strkey)
	{
		if (strkey != NULL || numkey != expected)
			return false;
		expected++;
	}
	ZEND_HASH_FOREACH_END();
	return true;
#endif
}

/*
 * Convert a zval to jsonb.  For containers, push begin/elements/end onto
 * the parse state; for scalars, push an element/value (or, at the top
 * level where *ps is NULL, return a palloc'd scalar JsonbValue).
 */
static JsonbValue *
zval_to_jsonbvalue(zval *val, JsonbParseState **ps, bool is_elem)
{
	JsonbValue	jb;
	JsonbValue *out = NULL;

	ZVAL_DEREF(val);

	switch (Z_TYPE_P(val))
	{
		case IS_ARRAY:
			if (plphp_array_is_list(Z_ARRVAL_P(val)))
			{
				zval	   *el;

				pushJsonbValue(ps, WJB_BEGIN_ARRAY, NULL);
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), el)
				{
					(void) zval_to_jsonbvalue(el, ps, true);
				}
				ZEND_HASH_FOREACH_END();
				return pushJsonbValue(ps, WJB_END_ARRAY, NULL);
			}
			else
			{
				zend_string *strkey;
				zend_ulong	numkey;
				zval	   *el;

				pushJsonbValue(ps, WJB_BEGIN_OBJECT, NULL);
				ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(val), numkey, strkey, el)
				{
					JsonbValue	key;

					key.type = jbvString;
					if (strkey != NULL)
					{
						key.val.string.val = pnstrdup(ZSTR_VAL(strkey),
													  ZSTR_LEN(strkey));
						key.val.string.len = ZSTR_LEN(strkey);
					}
					else
					{
						key.val.string.val = psprintf(UINT64_FORMAT,
													  (uint64) numkey);
						key.val.string.len = strlen(key.val.string.val);
					}
					pushJsonbValue(ps, WJB_KEY, &key);
					(void) zval_to_jsonbvalue(el, ps, false);
				}
				ZEND_HASH_FOREACH_END();
				return pushJsonbValue(ps, WJB_END_OBJECT, NULL);
			}

		case IS_NULL:
			jb.type = jbvNull;
			break;

		case IS_TRUE:
		case IS_FALSE:
			jb.type = jbvBool;
			jb.val.boolean = (Z_TYPE_P(val) == IS_TRUE);
			break;

		case IS_LONG:
			jb.type = jbvNumeric;
			jb.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
										 Int64GetDatum((int64) Z_LVAL_P(val))));
			break;

		case IS_DOUBLE:
			if (isinf(Z_DVAL_P(val)) || isnan(Z_DVAL_P(val)))
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("cannot convert infinity or NaN to jsonb")));
			jb.type = jbvNumeric;
			jb.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric,
										 Float8GetDatum(Z_DVAL_P(val))));
			break;

		case IS_STRING:
			jb.type = jbvString;
			jb.val.string.val = pnstrdup(Z_STRVAL_P(val), Z_STRLEN_P(val));
			jb.val.string.len = Z_STRLEN_P(val);
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot transform this PHP value to jsonb"),
					 errdetail("PHP type %d has no jsonb representation.",
							   (int) Z_TYPE_P(val))));
	}

	/* a scalar */
	if (*ps != NULL)
		out = pushJsonbValue(ps, is_elem ? WJB_ELEM : WJB_VALUE, &jb);
	else
	{
		out = (JsonbValue *) palloc(sizeof(JsonbValue));
		*out = jb;
	}

	return out;
}

/*
 * jsonb -> PHP (the FROM SQL transform function)
 */
PG_FUNCTION_INFO_V1(jsonb_to_plphp);

Datum
jsonb_to_plphp(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);

	PG_RETURN_POINTER(jsonb_container_to_zval(&in->root));
}

/*
 * PHP -> jsonb (the TO SQL transform function)
 */
PG_FUNCTION_INFO_V1(plphp_to_jsonb);

Datum
plphp_to_jsonb(PG_FUNCTION_ARGS)
{
	zval	   *in = (zval *) PG_GETARG_POINTER(0);
	JsonbParseState *ps = NULL;
	JsonbValue *out;

	out = zval_to_jsonbvalue(in, &ps, true);

	PG_RETURN_JSONB_P(JsonbValueToJsonb(out));
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
