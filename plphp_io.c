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

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
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


/* plphp_srf_htup_from_zval
 * 		Build a tuple from a zval and a TupleDesc, for a SRF.
 *
 * Like above, but we don't use the names of the array attributes;
 * rather we build the tuple in order.  Also, we get a MemoryContext
 * from the caller and just clean it at return, rather than building it each
 * time.
 */
HeapTuple
plphp_srf_htup_from_zval(zval *val, AttInMetadata *attinmeta,
						 MemoryContext cxt)
{
	MemoryContext	oldcxt;
	HeapTuple		ret;
	HashPosition	pos;
	char		  **values;
	zval		  **element;
	int				i = 0;

	oldcxt = MemoryContextSwitchTo(cxt);

	/*
	 * Use palloc0 to initialize values to NULL, just in case the user does
	 * not pass all needed attributes
	 */
	values = (char **) palloc0(attinmeta->tupdesc->natts * sizeof(char *));

	/*
	 * If the input zval is an array, build a tuple using each element as an
	 * attribute.  Exception: if the return tuple has a single element and
	 * it's an array type, use the whole array as a single value.
	 *
	 * If the input zval is a scalar, use it as an element directly.
	 */
	if (Z_TYPE_P(val) == IS_ARRAY)
	{
		if (attinmeta->tupdesc->natts == 1)
		{
			/* Is it an array? */
			if (attinmeta->tupdesc->attrs[0]->attndims != 0 ||
				!OidIsValid(get_element_type(attinmeta->tupdesc->attrs[0]->atttypid)))
			{
				zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(val), &pos);
				zend_hash_get_current_data_ex(Z_ARRVAL_P(val),
											  (void **) &element,
											  &pos);
				values[0] = plphp_zval_get_cstring(element[0], true, true);
			}
			else
				values[0] = plphp_zval_get_cstring(val, true, true);
		}
		else
		{
			/*
			 * Ok, it's an array and the return tuple has more than one
			 * attribute, so scan each array element.
			 */
			for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(val), &pos);
				 zend_hash_get_current_data_ex(Z_ARRVAL_P(val),
											   (void **) &element,
											   &pos) == SUCCESS;
				 zend_hash_move_forward_ex(Z_ARRVAL_P(val), &pos))
			{
				/* avoid overrunning the palloc'ed chunk */
				if (i >= attinmeta->tupdesc->natts)
				{
					elog(WARNING, "more elements in array than attributes in return type");
					break;
				}

				values[i++] = plphp_zval_get_cstring(element[0], true, true);
			}
		}
	}
	else
	{
		/* The passed zval is not an array -- use as the only attribute */
		if (attinmeta->tupdesc->natts != 1)
			ereport(ERROR,
					(errmsg("returned array does not correspond to "
							"declared return value")));

		values[0] = plphp_zval_get_cstring(val, true, true);
	}

	MemoryContextSwitchTo(oldcxt);

	ret = BuildTupleFromCStrings(attinmeta, values);

	MemoryContextReset(cxt);

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
plphp_convert_from_pg_array(char *input TSRMLS_DC)
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

	if (zend_symtable_find(array->value.ht,
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
			snprintf(ret, 64, "%ld", Z_LVAL_P(val));
			break;
		case IS_DOUBLE:
			ret = palloc(64);
			snprintf(ret, 64, "%f", Z_DVAL_P(val));
			break;
		case IS_BOOL:
			ret = palloc(8);
			snprintf(ret, 8, "%s", Z_BVAL_P(val) ? "true": "false");
			break;
		case IS_STRING:
			ret = palloc(Z_STRLEN_P(val) + 1);
			snprintf(ret, Z_STRLEN_P(val) + 1, "%s", 
					 Z_STRVAL_P(val));
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
 * plphp_build_tuple_argument
 *
 * Build a PHP array from all attributes of a given tuple
 */
zval *
plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	zval	   *output;
	Datum		attr;
	bool		isnull;
	char	   *attname;
	char	   *outputstr;
	HeapTuple	typeTup;
	Oid			typoutput;
	Oid			typioparam;

	MAKE_STD_ZVAL(output);
	array_init(output);

	for (i = 0; i < tupdesc->natts; i++)
	{
		/* Ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/* Get the attribute name */
		attname = tupdesc->attrs[i]->attname.data;

		/* Get the attribute value */
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/* If it is null, set it to undef in the hash. */
		if (isnull)
		{
			add_next_index_unset(output);
			continue;
		}

		/*
		 * Lookup the attribute type in the syscache for the output function
		 */
		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "cache lookup failed for type %u",
				 tupdesc->attrs[i]->atttypid);
		}

		typoutput = ((Form_pg_type) GETSTRUCT(typeTup))->typoutput;
		typioparam = getTypeIOParam(typeTup);
		ReleaseSysCache(typeTup);

		/* Append the attribute name and the value to the list. */
		outputstr =
			DatumGetCString(OidFunctionCall3(typoutput, attr,
											 ObjectIdGetDatum(typioparam),
											 Int32GetDatum(tupdesc->attrs[i]->atttypmod)));
		add_assoc_string(output, attname, outputstr, 1);
		pfree(outputstr);
	}

	return output;
}

/*
 * plphp_modify_tuple
 * 		Return the modified NEW tuple, for use as return value in a BEFORE
 * 		trigger.  outdata must point to the $_TD variable from the PHP
 * 		function.
 *
 * The tuple will be allocated in the current memory context and must be freed
 * by the caller.
 *
 * XXX Possible optimization: make this a global context that is not deleted,
 * but only reset each time this function is called.  (Think about triggers
 * calling other triggers though).
 */
HeapTuple
plphp_modify_tuple(zval *outdata, TriggerData *tdata)
{
	TupleDesc	tupdesc;
	HeapTuple	rettuple;
	zval	   *newtup;
	zval	  **element;
	char	  **vals;
	int			i;
	AttInMetadata *attinmeta;
	MemoryContext tmpcxt,
				  oldcxt;

	tmpcxt = AllocSetContextCreate(CurTransactionContext,
								   "PL/php NEW context",
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);

	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Fetch "new" from $_TD */
	if (zend_hash_find(outdata->value.ht,
					   "new", strlen("new") + 1,
					   (void **) &element) != SUCCESS)
		elog(ERROR, "$_TD['new'] not found");

	if (Z_TYPE_P(element[0]) != IS_ARRAY)
		elog(ERROR, "$_TD['new'] must be an array");
	newtup = element[0];

	/* Fetch the tupledesc and metadata */
	tupdesc = tdata->tg_relation->rd_att;
	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	i = zend_hash_num_elements(Z_ARRVAL_P(newtup));

	if (tupdesc->natts > i)
		ereport(ERROR,
				(errmsg("insufficient number of keys in $_TD['new']"),
				 errdetail("At least %d expected, %d found.",
						   tupdesc->natts, i)));

	vals = (char **) palloc(tupdesc->natts * sizeof(char *));

	/*
	 * For each attribute in the tupledesc, get its value from newtup and put
	 * it in an array of cstrings.
	 */
	for (i = 0; i < tupdesc->natts; i++)
	{
		zval  **element;
		char   *attname = NameStr(tupdesc->attrs[i]->attname);

		/* Fetch the attribute value from the zval */
		if (zend_symtable_find(newtup->value.ht, attname, strlen(attname) + 1,
						   	   (void **) &element) != SUCCESS)
			elog(ERROR, "$_TD['new'] does not contain attribute \"%s\"",
				 attname);

		vals[i] = plphp_zval_get_cstring(element[0], true, true);
	}

	/* Return to the original context so that the new tuple will survive */
	MemoryContextSwitchTo(oldcxt);

	/* Build the tuple */
	rettuple = BuildTupleFromCStrings(attinmeta, vals);

	/* Free the memory used */
	MemoryContextDelete(tmpcxt);

	return rettuple;
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
