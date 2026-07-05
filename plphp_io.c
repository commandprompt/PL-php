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

#include "postgres.h"
#include "plphp_io.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/memutils.h"

/*
 * plphp_zval_from_tuple
 *		 Build a PHP hash from a tuple.
 *
 * Returns a freshly emalloc'd zval; see the ownership note in plphp_io.h.
 */
zval *
plphp_zval_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	zval	   *array;

	array = (zval *) emalloc(sizeof(zval));
	array_init(array);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *attname = NameStr(att->attname);
		char	   *attdata;

		/* get its value */
		if ((attdata = SPI_getvalue(tuple, tupdesc, i + 1)) != NULL)
		{
			/* add_assoc_string copies the string in PHP 7+ */
			add_assoc_string(array, attname, attdata);
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
 * If zval doesn't contain any of the element names from the TupleDesc,
 * build a tuple from the first N elements. This allows us to accept
 * arrays in form array(1,2,3) as the result of functions with OUT arguments.
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
	bool			allempty = true;

	tmpcxt = AllocSetContextCreate(TopTransactionContext,
								   "htup_from_zval cxt",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	values = (char **) palloc(tupdesc->natts * sizeof(char *));

	for (i = 0; i < tupdesc->natts; i++)
	{
		char   *key = SPI_fname(tupdesc, i + 1);
		zval   *scalarval = plphp_array_get_elem(val, key);

		values[i] = plphp_zval_get_cstring(scalarval, true, true);
		/*
		 * Reset the flag if even one of the keys actually exists,
		 * even if it is NULL.
		 */
		if (scalarval != NULL)
			allempty = false;
	}

	/*
	 * None of the names from the tuple exists; try to get the first N array
	 * elements and assign them to the tuple.
	 */
	if (allempty)
	{
		zval   *element;

		i = 0;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), element)
		{
			if (i >= tupdesc->natts)
				break;
			values[i++] = plphp_zval_get_cstring(element, true, true);
		}
		ZEND_HASH_FOREACH_END();
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
	char		  **values;
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
			Form_pg_attribute att = TupleDescAttr(attinmeta->tupdesc, 0);

			/* Is it an array? */
			if (att->attndims != 0 ||
				!OidIsValid(get_element_type(att->atttypid)))
			{
				zval   *element = NULL;

				/* grab the first element of the array */
				ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), element)
				{
					break;
				}
				ZEND_HASH_FOREACH_END();
				values[0] = plphp_zval_get_cstring(element, true, true);
			}
			else
				values[0] = plphp_zval_get_cstring(val, true, true);
		}
		else
		{
			zval   *element;

			/*
			 * Ok, it's an array and the return tuple has more than one
			 * attribute, so scan each array element.
			 */
			ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(val), element)
			{
				/* avoid overrunning the palloc'ed chunk */
				if (i >= attinmeta->tupdesc->natts)
				{
					elog(WARNING, "more elements in array than attributes in return type");
					break;
				}

				values[i++] = plphp_zval_get_cstring(element, true, true);
			}
			ZEND_HASH_FOREACH_END();
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
 * plphp_append_array_string
 * 		Append one string element to an array literal under construction,
 * 		double-quoted, with embedded quotes and backslashes escaped.
 */
static void
plphp_append_array_string(StringInfo str, const char *s, size_t len)
{
	size_t		i;

	appendStringInfoChar(str, '"');
	for (i = 0; i < len; i++)
	{
		if (s[i] == '"' || s[i] == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, s[i]);
	}
	appendStringInfoChar(str, '"');
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
	zval	   *element;
	int			i = 0;
	StringInfoData	str;

	initStringInfo(&str);

	arr_size = zend_hash_num_elements(Z_ARRVAL_P(array));

	appendStringInfoChar(&str, '{');
	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(array), element)
		{
			char *tmp;

			switch (Z_TYPE_P(element))
			{
				case IS_LONG:
					appendStringInfo(&str, "%ld", (long) Z_LVAL_P(element));
					break;
				case IS_DOUBLE:
					appendStringInfo(&str, "%f", Z_DVAL_P(element));
					break;
				case IS_TRUE:
					appendStringInfoString(&str, "\"true\"");
					break;
				case IS_FALSE:
					appendStringInfoString(&str, "\"false\"");
					break;
				case IS_NULL:
					appendStringInfoString(&str, "NULL");
					break;
				case IS_STRING:
					plphp_append_array_string(&str, Z_STRVAL_P(element),
											  Z_STRLEN_P(element));
					break;
				case IS_ARRAY:
					tmp = plphp_convert_to_pg_array(element);
					appendStringInfoString(&str, tmp);
					pfree(tmp);
					break;
				default:
					elog(ERROR, "unrecognized element type %d",
						 Z_TYPE_P(element));
			}

			if (i != arr_size - 1)
				appendStringInfoChar(&str, ',');
			i++;
		}
		ZEND_HASH_FOREACH_END();
	}

	appendStringInfoChar(&str, '}');

	return str.data;
}

/*
 * plphp_parse_array_body
 * 		Parse the contents of an array literal, starting just past an opening
 * 		brace, adding the elements to arr.  Returns the position just past the
 * 		matching closing brace.
 *
 * The input always comes from an array type's output function, so the
 * delimiter is assumed to be a comma (true for every built-in type except
 * box) and the syntax is trusted to be well-formed.
 *
 * Quoted elements are always strings.  Unquoted elements are NULL, or are
 * converted to PHP int/float when they look like numbers (preserving the
 * semantics of the old zend_eval_string-based conversion), and are strings
 * otherwise.
 */
static const char *
plphp_parse_array_body(const char *p, zval *arr)
{
	StringInfoData buf;

	initStringInfo(&buf);

	for (;;)
	{
		if (*p == '\0')
			elog(ERROR, "malformed array literal");
		else if (*p == '}')		/* the array is empty */
		{
			p++;
			break;
		}
		else if (*p == '{')		/* nested sub-array */
		{
			zval		sub;

			array_init(&sub);
			p = plphp_parse_array_body(p + 1, &sub);
			add_next_index_zval(arr, &sub);
		}
		else if (*p == '"')		/* quoted element: always a string */
		{
			resetStringInfo(&buf);
			for (p++; *p != '"'; p++)
			{
				if (*p == '\0')
					elog(ERROR, "malformed array literal");
				if (*p == '\\')
					p++;
				appendStringInfoChar(&buf, *p);
			}
			p++;				/* skip the closing quote */
			add_next_index_stringl(arr, buf.data, buf.len);
		}
		else					/* unquoted element */
		{
			char	   *end;
			long		lval;
			double		dval;

			resetStringInfo(&buf);
			while (*p != ',' && *p != '}' && *p != '\0')
				appendStringInfoChar(&buf, *p++);

			errno = 0;
			if (strcmp(buf.data, "NULL") == 0)
				add_next_index_null(arr);
			else if (buf.len > 0 &&
					 (lval = strtol(buf.data, &end, 10), *end == '\0') &&
					 errno != ERANGE)
				add_next_index_long(arr, (zend_long) lval);
			else if (buf.len > 0 &&
					 (dval = strtod(buf.data, &end), *end == '\0'))
				add_next_index_double(arr, dval);
			else
				add_next_index_stringl(arr, buf.data, buf.len);
		}

		/* between elements: expect a comma or the closing brace */
		if (*p == ',')
			p++;
		else if (*p == '}')
		{
			p++;
			break;
		}
		else
			elog(ERROR, "malformed array literal");
	}

	pfree(buf.data);
	return p;
}

/*
 * plphp_convert_from_pg_array
 * 		Convert a Postgres text array representation to a PHP array
 * 		(zval type thing).
 *
 * Returns a freshly emalloc'd zval; see the ownership note in plphp_io.h.
 */
zval *
plphp_convert_from_pg_array(char *input)
{
	zval	   *retval;
	const char *p = input;

	/* skip any dimension decoration, e.g. "[0:2]={...}" */
	while (*p != '\0' && *p != '{')
		p++;
	if (*p != '{')
		elog(ERROR, "expected an array literal, got \"%s\"", input);

	retval = (zval *) emalloc(sizeof(zval));
	array_init(retval);
	(void) plphp_parse_array_body(p + 1, retval);

	return retval;
}

/*
 * plphp_array_get_elem
 * 		Return a *borrowed* pointer to the array element with the given key,
 * 		or NULL if there is no such element.
 */
zval *
plphp_array_get_elem(zval *array, char *key)
{
	if (!array)
		elog(ERROR, "passed zval is not a valid pointer");
	if (Z_TYPE_P(array) != IS_ARRAY)
		elog(ERROR, "passed zval is not an array");

	/* zend_symtable_str_find honours integer-like string keys */
	return zend_symtable_str_find(Z_ARRVAL_P(array), key, strlen(key));
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

	/* Resolve references (e.g. INOUT arguments are passed by reference) */
	ZVAL_DEREF(val);

	switch (Z_TYPE_P(val))
	{
		case IS_NULL:
			return NULL;
		case IS_LONG:
			ret = palloc(64);
			snprintf(ret, 64, "%ld", (long) Z_LVAL_P(val));
			break;
		case IS_DOUBLE:
			ret = palloc(64);
			snprintf(ret, 64, "%f", Z_DVAL_P(val));
			break;
		case IS_TRUE:
			ret = palloc(8);
			strcpy(ret, "true");
			break;
		case IS_FALSE:
			ret = palloc(8);
			strcpy(ret, "false");
			break;
		case IS_STRING:
			ret = palloc(Z_STRLEN_P(val) + 1);
			memcpy(ret, Z_STRVAL_P(val), Z_STRLEN_P(val));
			ret[Z_STRLEN_P(val)] = '\0';
			break;
		case IS_ARRAY:
			if (!do_array)
				elog(ERROR, "can't stringize array value");
			ret = plphp_convert_to_pg_array(val);
			break;
		default:
			/* keep compiler quiet */
			ret = NULL;
			elog(ERROR, "can't stringize value of type %d", Z_TYPE_P(val));
	}

	return ret;
}

/*
 * plphp_build_tuple_argument
 *
 * Build a PHP array from all attributes of a given tuple.
 *
 * Returns a freshly emalloc'd zval; see the ownership note in plphp_io.h.
 */
zval *
plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	zval	   *output;
	Datum		attr_datum;
	bool		isnull;
	char	   *attname;
	char	   *outputstr;
	HeapTuple	typeTup;
	Oid			typoutput;

	output = (zval *) emalloc(sizeof(zval));
	array_init(output);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/* Ignore dropped attributes */
		if (att->attisdropped)
			continue;

		/* Get the attribute name */
		attname = NameStr(att->attname);

		/* Get the attribute value */
		attr_datum = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/* If it is null, set it to null in the hash. */
		if (isnull)
		{
			add_assoc_null(output, attname);
			continue;
		}

		/*
		 * Lookup the attribute type in the syscache for the output function
		 */
		typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typeTup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);

		typoutput = ((Form_pg_type) GETSTRUCT(typeTup))->typoutput;
		ReleaseSysCache(typeTup);

		/* Append the attribute name and the value to the list. */
		outputstr = OidOutputFunctionCall(typoutput, attr_datum);
		add_assoc_string(output, attname, outputstr);
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
 */
HeapTuple
plphp_modify_tuple(zval *outdata, TriggerData *tdata)
{
	TupleDesc	tupdesc;
	HeapTuple	rettuple;
	zval	   *newtup;
	zval	   *element;
	char	  **vals;
	int			i;
	AttInMetadata *attinmeta;
	MemoryContext tmpcxt,
				  oldcxt;

	tmpcxt = AllocSetContextCreate(CurTransactionContext,
								   "PL/php NEW context",
								   ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Fetch "new" from $_TD */
	element = zend_hash_str_find(Z_ARRVAL_P(outdata), "new", strlen("new"));
	if (element == NULL)
		elog(ERROR, "$_TD['new'] not found");

	if (Z_TYPE_P(element) != IS_ARRAY)
		elog(ERROR, "$_TD['new'] must be an array");
	newtup = element;

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
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char   *attname = NameStr(att->attname);
		zval   *el;

		/* Fetch the attribute value from the zval */
		el = zend_symtable_str_find(Z_ARRVAL_P(newtup), attname, strlen(attname));
		if (el == NULL)
			elog(ERROR, "$_TD['new'] does not contain attribute \"%s\"",
				 attname);

		vals[i] = plphp_zval_get_cstring(el, true, true);
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
