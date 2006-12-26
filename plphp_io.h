/*
 * $Id$
 */

#ifndef PLPHP_IO_H
#define PLPHP_IO_H

/* PostgreSQL stuff */
#include "postgres.h"

/* This is defined in tcop/dest.h but PHP defines it again */
#define Debug DestDebug

#include "access/heapam.h"
#include "commands/trigger.h"
#include "funcapi.h"

/*
 * These are defined again in php.h, so undef them to avoid some
 * cpp warnings.
 */
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#undef Debug

/* PHP stuff */
#include "php.h"

zval *plphp_zval_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
HeapTuple plphp_htup_from_zval(zval *val, TupleDesc tupdesc);
HeapTuple plphp_srf_htup_from_zval(zval *val, AttInMetadata *attinmeta,
						 MemoryContext cxt);
char *plphp_convert_to_pg_array(zval *array);
zval *plphp_convert_from_pg_array(char *input);
zval *plphp_array_get_elem(zval* array, char *key);
char *plphp_zval_get_cstring(zval *val, bool do_array, bool null_ok);
zval *plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
HeapTuple plphp_modify_tuple(zval *outdata, TriggerData *tdata);

#endif /* PLPHP_IO_H */
