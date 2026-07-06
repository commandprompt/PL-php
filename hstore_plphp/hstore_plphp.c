/**********************************************************************
 * hstore_plphp.c - TRANSFORM between hstore and PL/php arrays.
 *
 * hstore_to_plphp (FROM SQL) converts an hstore datum into a PHP
 * associative array of string keys to string (or null) values.
 * plphp_to_hstore (TO SQL) converts a PHP array back: keys and scalar
 * values are stringified, and a PHP null value becomes an hstore NULL.
 *
 * Like hstore_plperl (and unlike touching hstore's internal
 * representation), this module converts through the hstore extension's
 * own SQL-callable functions, hstore_to_array(hstore) and hstore(text[]),
 * whose OIDs are resolved at run time.  The hstore type is found through
 * the pg_transform row that points at this very function, so the lookup
 * is independent of schemas and search_path.  The resolved OID is cached
 * in fn_extra.
 *
 * The PHP interpreter is owned and initialized by PL/php; these functions
 * only run inside a PL/php function call, so it is always up.
 *
 * This software is copyright (c) Command Prompt Inc.  Same license as
 * PL/php itself; see plphp.c.
 **********************************************************************/

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#else
#include "access/heapam.h"
#define table_open(rel, lock) heap_open(rel, lock)
#define table_close(rel, lock) heap_close(rel, lock)
#endif
#include "catalog/pg_proc.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*
 * These are defined again in php.h, so undef them to avoid some
 * cpp warnings (same dance as jsonb_plphp.c).
 */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#include "php.h"
#include "Zend/zend_API.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(hstore_to_plphp);
PG_FUNCTION_INFO_V1(plphp_to_hstore);

/*
 * The hstore type OID, found via the pg_transform row whose FromSQL (or
 * ToSQL) function is this transform function itself.
 */
static Oid
hstore_type_from_transform(Oid trf_fn, bool fromsql)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rel = table_open(TransformRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_transform t = (Form_pg_transform) GETSTRUCT(tup);

		if ((fromsql ? t->trffromsql : t->trftosql) == trf_fn)
		{
			result = t->trftype;
			break;
		}
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (!OidIsValid(result))
		elog(ERROR, "could not find pg_transform entry for function %u", trf_fn);
	return result;
}

/*
 * The OID of an hstore-extension function living in the hstore type's own
 * namespace: hstore_to_array(hstore) or hstore(text[]).
 */
static Oid
lookup_hstore_fn(Oid hstore_oid, const char *name, Oid argtype)
{
	HeapTuple	typtup;
	Oid			nsp;
	oidvector  *argv;
	HeapTuple	ftup;
	Oid			result;

	typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(hstore_oid));
	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", hstore_oid);
	nsp = ((Form_pg_type) GETSTRUCT(typtup))->typnamespace;
	ReleaseSysCache(typtup);

	argv = buildoidvector(&argtype, 1);
	ftup = SearchSysCache3(PROCNAMEARGSNSP, CStringGetDatum(name),
						   PointerGetDatum(argv), ObjectIdGetDatum(nsp));
	if (!HeapTupleIsValid(ftup))
		elog(ERROR, "could not find the hstore extension's %s function", name);
#if PG_VERSION_NUM >= 120000
	result = ((Form_pg_proc) GETSTRUCT(ftup))->oid;
#else
	result = HeapTupleGetOid(ftup);
#endif
	ReleaseSysCache(ftup);
	return result;
}

/* Resolve (once per FmgrInfo) the conversion function this direction uses. */
static Oid
conversion_fn(FunctionCallInfo fcinfo, bool fromsql)
{
	Oid		   *cached = (Oid *) fcinfo->flinfo->fn_extra;

	if (cached == NULL)
	{
		Oid			hstore_oid = hstore_type_from_transform(fcinfo->flinfo->fn_oid,
														fromsql);

		cached = (Oid *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(Oid));
		if (fromsql)
			*cached = lookup_hstore_fn(hstore_oid, "hstore_to_array",
									   hstore_oid);
		else
			*cached = lookup_hstore_fn(hstore_oid, "hstore", TEXTARRAYOID);
		fcinfo->flinfo->fn_extra = cached;
	}
	return *cached;
}

/* ---------------------------------------------------------------------
 * hstore -> PHP associative array
 * ------------------------------------------------------------------- */

Datum
hstore_to_plphp(PG_FUNCTION_ARGS)
{
	Oid			to_array = conversion_fn(fcinfo, true);
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	int			i;
	zval	   *z;

	/* hstore_to_array: alternating key, value; a NULL value stays NULL */
	arr = DatumGetArrayTypeP(OidFunctionCall1(to_array, PG_GETARG_DATUM(0)));
	deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, &nulls, &nelems);

	z = (zval *) emalloc(sizeof(zval));
	array_init(z);

	for (i = 0; i + 1 < nelems; i += 2)
	{
		text	   *kt = DatumGetTextPP(elems[i]);
		char	   *key = pnstrdup(VARDATA_ANY(kt), VARSIZE_ANY_EXHDR(kt));
		zval		v;

		if (nulls[i + 1])
			ZVAL_NULL(&v);
		else
		{
			text	   *vt = DatumGetTextPP(elems[i + 1]);

			ZVAL_STRINGL(&v, VARDATA_ANY(vt), VARSIZE_ANY_EXHDR(vt));
		}
		add_assoc_zval(z, key, &v);
		pfree(key);
	}

	PG_RETURN_POINTER(z);
}

/* ---------------------------------------------------------------------
 * PHP array -> hstore
 * ------------------------------------------------------------------- */

Datum
plphp_to_hstore(PG_FUNCTION_ARGS)
{
	Oid			from_array = conversion_fn(fcinfo, false);
	zval	   *in = (zval *) PG_GETARG_POINTER(0);
	HashTable  *ht;
	Datum	   *elems;
	bool	   *nulls;
	int			npairs;
	int			next;
	int			dims[1];
	int			lbs[1] = {1};
	ArrayType  *arr;
	zend_string *strkey;
	zend_ulong	numkey;
	zval	   *el;

	ZVAL_DEREF(in);
	if (Z_TYPE_P(in) != IS_ARRAY)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot transform PHP value to hstore"),
				 errhint("An hstore value is built from a PHP array.")));

	ht = Z_ARRVAL_P(in);
	npairs = zend_hash_num_elements(ht);
	elems = (Datum *) palloc(npairs * 2 * sizeof(Datum));
	nulls = (bool *) palloc(npairs * 2 * sizeof(bool));
	next = 0;

	ZEND_HASH_FOREACH_KEY_VAL(ht, numkey, strkey, el)
	{
		/* key: a string key as-is, an integer key rendered as text */
		if (strkey != NULL)
			elems[next] = PointerGetDatum(cstring_to_text_with_len(ZSTR_VAL(strkey),
																   ZSTR_LEN(strkey)));
		else
		{
			char	   *k = psprintf(ZEND_LONG_FMT, (zend_long) numkey);

			elems[next] = PointerGetDatum(cstring_to_text(k));
			pfree(k);
		}
		nulls[next] = false;
		next++;

		/* value: PHP null -> hstore NULL; scalars stringified; else reject */
		ZVAL_DEREF(el);
		if (Z_TYPE_P(el) == IS_NULL)
		{
			elems[next] = (Datum) 0;
			nulls[next] = true;
		}
		else if (Z_TYPE_P(el) == IS_ARRAY || Z_TYPE_P(el) == IS_OBJECT)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot transform nested PHP value to an hstore value"),
					 errhint("hstore values are text or null; stringify first.")));
		else
		{
			zend_string *vstr = zval_get_string(el);

			elems[next] = PointerGetDatum(cstring_to_text_with_len(ZSTR_VAL(vstr),
																   ZSTR_LEN(vstr)));
			nulls[next] = false;
			zend_string_release(vstr);
		}
		next++;
	}
	ZEND_HASH_FOREACH_END();

	dims[0] = next;
	arr = construct_md_array(elems, nulls, 1, dims, lbs,
							 TEXTOID, -1, false, 'i');

	PG_RETURN_DATUM(OidFunctionCall1(from_array, PointerGetDatum(arr)));
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
