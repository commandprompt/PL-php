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
#include "lib/stringinfo.h"

/*
 * plphp_hash_from_tuple
 *
 *		 Build a PHP hash from a tuple
 */
zval *
plphp_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
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
			add_assoc_string(array, attname, attdata, 1);
		else
			add_assoc_null(array, attname);
	}
	return array;
}

/*
 * plphp_convert_to_pg_array
 *
 * 		Convert a zval into a Postgres text array representation.
 *
 * The return value is palloc'ed in the current memory context and
 * must be freed by the caller.
 */
char *
plphp_convert_to_pg_array(zval *array)
{
	int			arr_size = 0;
	zval	  **element;
	HashPosition pos;
	int			i = 0;
	StringInfoData	str;
	
	initStringInfo(&str);

	arr_size = plphp_attr_count(array);

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
					appendStringInfo(&str, "%e", element[0]->value.dval);
					break;
				case IS_STRING:
					appendStringInfo(&str, "%s", element[0]->value.str.val);
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
 *
 * 		Convert a Postgres text array representation to a PHP array
 * 		(zval type thing).
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
			appendStringInfo(&str, "%s", "array(");
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

zval *
plphp_get_pelem(zval *array, char *key)
{
	zval	  **element;
	HashPosition pos;

	if (!array)
		return NULL;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
			{
				if (strcasecmp(pos->arKey, key) == 0)
					return element[0];
			}
		}
	}
	return NULL;
}

char *
plphp_get_elem(zval *array, char *key)
{
	zval	   *element;
	char	   *ret;

	element = plphp_get_pelem(array, key);

	if ((element != NULL) && (Z_TYPE_P(element) != IS_ARRAY))
	{
		if (Z_TYPE_P(element) == IS_LONG)
		{
			ret = palloc(64);
			sprintf(ret, "%ld", element->value.lval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_DOUBLE)
		{
			ret = palloc(64);
			sprintf(ret, "%e", element->value.dval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_STRING)
			return element->value.str.val;
	}
	return NULL;
}

/*
 * plphp_attr_count
 * 		Return the number of elements in the passed array.
 *
 * Note that it doesn't count "second level" arrays, so if it gets
 * e.g. an array of two arrays, it returns 2.
 */
int
plphp_attr_count(zval *array)
{
	return zend_hash_num_elements(Z_ARRVAL_P(array));
}

/*
 * plphp_get_new
 *
 * 		Return a pointer to the zval of the "new" element in the passed
 * 		array.  This is used to rebuild the return tuple in the case of
 * 		a BEFORE-event trigger that modifies the NEW pseudo-tuple.
 */
zval *
plphp_get_new(zval *array)
{
	zval	  **element;

	if (zend_hash_find(array->value.ht,
					   "new", strlen("new") + 1,
					   (void **) &element) == SUCCESS)
	{
		if (Z_TYPE_P(element[0]) == IS_ARRAY)
			return element[0];

		elog(ERROR, "plphp: field $_TD['new'] must be an array");
	}

	elog(ERROR, "plphp: field $_TD['new'] not found");

	/* keep compiler quiet */
	return NULL;
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
