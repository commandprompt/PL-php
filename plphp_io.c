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
 */
int
plphp_convert_to_pg_array(zval * array, char *buffer)
{
	int			arr_size = 0;
	zval	  **element;
	HashPosition pos;
	char	   *cElem;
	int			i = 0;

	arr_size = plphp_attr_count(array);

	strcat(buffer, "{");
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
				switch (Z_TYPE_P(element[0]))
				{
					case IS_LONG:
						cElem = palloc(255);
						sprintf(cElem, "%li", element[0]->value.lval);
						strcat(buffer, cElem);
						pfree(cElem);
						break;
					case IS_DOUBLE:
						cElem = palloc(255);
						sprintf(cElem, "%e", element[0]->value.dval);
						strcat(buffer, cElem);
						pfree(cElem);
						break;
					case IS_STRING:
						strcat(buffer, element[0]->value.str.val);
						break;
				}
			}
			else
			{
				plphp_convert_to_pg_array(element[0], buffer);
			}
			if (i != arr_size - 1)
				strcat(buffer, ",");
			i++;
		}
	}
	strcat(buffer, "}");
	return 1;
}

/*
 * plphp_convert_from_pg_array
 *
 * 		Convert a Postgres text array representation to a PHP "array( ... )"
 * 		construct.
 */
zval *
plphp_convert_from_pg_array(char *input)
{
	zval	   *retval = NULL;
	char	   *work;
	int			i,
				arr_cnt = 0;
	char		buff[2];

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	for (i = 0; i < strlen(input); i++)
	{
		if (input[i] == '{')
			arr_cnt++;
	}

	work = (char *) palloc(strlen(input) + arr_cnt * 5 + 2);

	sprintf(work, "array(");

	for (i = 1; i < strlen(input); i++)
	{
		if (input[i] == '{')
			strcat(work, "array(");
		else if (input[i] == '}')
			strcat(work, ")");
		else
		{
			sprintf(buff, "%c", input[i]);
			strcat(work, buff);
		}
	}

	strcat(work, ";");

	if (zend_eval_string(work, retval,
						 "plphp array input parameter" TSRMLS_CC) == FAILURE)
		elog(ERROR, "plphp: convert to internal representation failure");

	return retval;
}

zval *
plphp_get_row(zval * array, int index)
{
	zval	  **element;
	HashPosition pos;
	int			row = 0;

	if (NULL != array)
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
					if (row == index)
						return element[0];
					row++;
				}
				else
				{
					if (row == index)
						return element[0];
					row++;
				}
			}
		}
	return NULL;
}

char *
plphp_get_elem(zval * array, char *key)
{
	zval	   *element;
	char	   *ret;

	ret = palloc(64);

	element = plphp_get_pelem(array, key);

	if ((element != NULL) && (Z_TYPE_P(element) != IS_ARRAY))
	{
		if (Z_TYPE_P(element) == IS_LONG)
		{
			sprintf(ret, "%ld", element->value.lval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_DOUBLE)
		{
			sprintf(ret, "%e", element->value.dval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_STRING)
			return element->value.str.val;
	}
	return NULL;
}

int
plphp_attr_count(zval *array)
{
	/* WARNING -- this only works on simple arrays */
	return zend_hash_num_elements(Z_ARRVAL_P(array));
}

/*
 * plphp_get_rows_num
 *
 * 		Return the number of "tuples" in the given zval.
 *
 * Note that if it's a simple array of scalar values, it's considered
 * a single tuple.  If it's an array of arrays, then it's considered
 * to be a set of tuples, and we count the number of elements of that
 * array that are in turn arrays.
 *
 * So constructs like
 * array(1, 2, 3)
 * array(array(1, 2, 3))
 *
 * return 1, while
 *
 * array(array(1, 2, 3), array(1, 2, 3))
 * returns 2, as does
 * array(array(1, 2, 3), 2, array(1, 2, 3))
 * (because the middle element is not an array, so it's not counted!)
 * Note that the latter is bogus ...
 */
int
plphp_get_rows_num(zval *array)
{
	zval	  **element;
	HashPosition pos;
	int			rows = 0;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) == IS_ARRAY)
				rows++;
		}

	}
	else
		elog(ERROR, "plphp: wrong type: %i", array->type);

	return ((rows == 0) ? 1 : rows);
}

int
plphp_attr_count_r(zval *array)
{
	zval	  **element;
	HashPosition pos;
	int			ret = 0;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
				ret++;
			else
				ret += plphp_attr_count_r(element[0]);
		}
	}
	return ret;
}

char **
plphp_get_attr_name(zval * array)
{
	char	  **rv;
	zval	  **element;
	HashPosition pos;
	int			cc = 0,
				i = 0;

	cc = plphp_attr_count(array);
	rv = (char **) palloc(cc * sizeof(char *));

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
				rv[i] = (char *) palloc(pos->nKeyLength * sizeof(char));
				rv[i] = pos->arKey;
				i++;
			}
			else
				elog(ERROR, "plphp: input type must not be an array");
		}
	}
	else
		elog(ERROR, "plphp: input type must be an array");
	return rv;
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

	if (zend_hash_find
		(array->value.ht, "new", sizeof("new"),
		 (void **) &element) == SUCCESS)
	{
		if (Z_TYPE_P(element[0]) == IS_ARRAY)
			return element[0];
		else
			elog(ERROR, "plphp: field $_TD['new'] must be an array");
	}
	else
		elog(ERROR, "plphp: field $_TD['new'] not found");
	return NULL;
}

zval *
plphp_get_pelem(zval *array, char *key)
{
	zval	  **element;
	HashPosition pos;

	if (NULL == array)
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

/*
 * vim:ts=4:sw=4:cino=(0
 */
