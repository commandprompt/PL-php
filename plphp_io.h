/*
 * $Id$
 */

#ifndef PLPHP_IO_H
#define PLPHP_IO_H

/* PostgreSQL stuff */
#include "postgres.h"
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


/* PHP stuff */
#include "php.h"

/*
 * Memory-ownership convention (PHP 8):
 *
 * The functions that return a "zval *" (plphp_zval_from_tuple,
 * plphp_convert_from_pg_array, plphp_build_tuple_argument) return a freshly
 * emalloc'd zval container holding an owned value.  The caller takes ownership
 * and must dispose of it exactly once, either by handing the value off to a
 * consumer that adopts it (add_assoc_zval / add_next_index_zval /
 * ZVAL_COPY_VALUE into return_value) and then efree()'ing the container, or by
 * calling zval_ptr_dtor() followed by efree() to discard it outright.
 *
 * plphp_array_get_elem returns a *borrowed* pointer into an existing array
 * (or NULL); it must not be freed.
 */
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
