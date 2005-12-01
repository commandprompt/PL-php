/**********************************************************************
 * plphp_io.c
 *
 * Support functions for PL/php -- mainly functions to convert stuff
 * from the PHP representation to PostgreSQL representation and vice
 * versa, either text or binary representations.
 *
 * $Id$
 *
 **********************************************************************/

#include "plphp_io.h"

#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/memutils.h"

/*
 * plphp_zval_from_tuple
 *		 Build a PHP hash from a tuple.
 */
zval *
plphp_zval_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *attname = NULL;
	zval	   *array;

	MAKE_STD_ZVAL(array);
	array_init(array);

	for (i = 0; i < tupdesc->natts; i++)
	{
		char *attdata;

		/* Get the attribute name */
		attname = tupdesc->attrs[i]->attname.data;

		/* and get its value */
		if ((attdata = SPI_getvalue(tuple, tupdesc, i + 1)) != NULL)
		{
			/* "true" means strdup the string */
			add_assoc_string(array, attname, attdata, true);
			pfree(attdata);
		}
		else
			add_assoc_null(array, attname);
	}
	return array;
}

/*
 * plphp_htup_from_zval
 * 		Build a HeapTuple from a zval (which must be an array) and a TupleDesc.
 *
 * The return HeapTuple is allocated in the current memory context and must
 * be freed by the caller.
 *
 * XXX -- possible optimization: keep the memory context created and only
 * reset it between calls.
 */
HeapTuple
plphp_htup_from_zval(zval *val, TupleDesc tupdesc)
{
	MemoryContext	oldcxt;
	MemoryContext	tmpcxt;
	HeapTuple		ret;
	AttInMetadata  *attinmeta;
	char		  **values;
	int				i;

	tmpcxt = AllocSetContextCreate(TopTransactionContext,
								   "htup_from_zval cxt",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	values = (char **) palloc(tupdesc->natts * sizeof(char *));

	for (i = 0; i < tupdesc->natts; i++)
	{
		char   *key = SPI_fname(tupdesc, i + 1);

		/* may be NULL but we don't care */
		values[i] = plphp_zval_get_cstring(plphp_array_get_elem(val, key),
										   true, true);
	}

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	MemoryContextSwitchTo(oldcxt);
	ret = BuildTupleFromCStrings(attinmeta, values);

	MemoryContextDelete(tmpcxt);

	return ret;
}

/*
 * plphp_convert_to_pg_array
 * 		Convert a zval into a Postgres text array representation.
 *
 * The return value is palloc'ed in the current memory context and
 * must be freed by the caller.
 */
char *
plphp_convert_to_pg_array(zval *array)
{
	int			arr_size;
	zval	  **element;
	int			i = 0;
	HashPosition 	pos;
	StringInfoData	str;
	
	initStringInfo(&str);

	arr_size = zend_hash_num_elements(Z_ARRVAL_P(array));

	appendStringInfoChar(&str, '{');
	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			char *tmp;

			switch (Z_TYPE_P(element[0]))
			{
				case IS_LONG:
					appendStringInfo(&str, "%li", element[0]->value.lval);
					break;
				case IS_DOUBLE:
					appendStringInfo(&str, "%f", element[0]->value.dval);
					break;
				case IS_STRING:
					appendStringInfo(&str, "\"%s\"", element[0]->value.str.val);
					break;
				case IS_ARRAY:
					tmp = plphp_convert_to_pg_array(element[0]);
					appendStringInfo(&str, "%s", tmp);
					pfree(tmp);
					break;
				default:
					elog(ERROR, "unrecognized element type %d",
						 Z_TYPE_P(element[0]));
			}

			if (i != arr_size - 1)
				appendStringInfoChar(&str, ',');
			i++;
		}
	}

	appendStringInfoChar(&str, '}');

	return str.data;
}

/*
 * plphp_convert_from_pg_array
 * 		Convert a Postgres text array representation to a PHP array
 * 		(zval type thing).
 *
 * FIXME -- does not work if there are embedded {'s in the input value.
 *
 * FIXME -- does not correctly quote/dequote the values
 */
zval *
plphp_convert_from_pg_array(char *input)
{
	zval	   *retval = NULL;
	int			i;
	StringInfoData str;
	
	initStringInfo(&str);

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	for (i = 0; i < strlen(input); i++)
	{
		if (input[i] == '{')
			appendStringInfoString(&str, "array(");
		else if (input[i] == '}')
			appendStringInfoChar(&str, ')');
		else
			appendStringInfoChar(&str, input[i]);
	}
	appendStringInfoChar(&str, ';');

	if (zend_eval_string(str.data, retval,
						 "plphp array input parameter" TSRMLS_CC) == FAILURE)
		elog(ERROR, "plphp: convert to internal representation failure");

	pfree(str.data);

	return retval;
}

/*
 * plphp_array_get_elem
 * 		Return a pointer to the array element with the given key
 */
zval *
plphp_array_get_elem(zval *array, char *key)
{
	zval	  **element;

	if (!array)
		elog(ERROR, "passed zval is not a valid pointer");
	if (Z_TYPE_P(array) != IS_ARRAY)
		elog(ERROR, "passed zval is not an array");

	if (zend_hash_find(array->value.ht,
					   key,
					   strlen(key) + 1,
					   (void **) &element) != SUCCESS)
		return NULL;

	return element[0];
}

/*
 * zval_get_cstring
 *		Get a C-string representation of a zval.
 *
 * All return values, except those that are NULL, are palloc'ed in the current
 * memory context and must be freed by the caller.
 *
 * If the do_array parameter is false, then array values will not be converted
 * and an error will be raised instead.
 *
 * If the null_ok parameter is true, we will return NULL for a NULL zval.
 * Otherwise we raise an error.
 */
char *
plphp_zval_get_cstring(zval *val, bool do_array, bool null_ok)
{
	char *ret;

	if (!val)
	{
		if (null_ok)
			return NULL;
		else
			elog(ERROR, "invalid zval pointer");
	}

	switch (Z_TYPE_P(val))
	{
		case IS_NULL:
			return NULL;
		case IS_LONG:
			ret = palloc(64);
			snprintf(ret, 64, "%ld", val->value.lval);
			break;
		case IS_DOUBLE:
			ret = palloc(64);
			snprintf(ret, 64, "%f", val->value.dval);
			break;
		case IS_STRING:
			ret = palloc(val->value.str.len + 1);
			snprintf(ret, val->value.str.len + 1, "%s", 
					 val->value.str.val);
			break;
		case IS_ARRAY:
			if (!do_array)
				elog(ERROR, "can't stringize array value");
			ret = plphp_convert_to_pg_array(val);
			break;
		default:
			/* keep compiler quiet */
			ret = NULL;
			elog(ERROR, "can't stringize value of type %d", val->type);
	}

	return ret;
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
