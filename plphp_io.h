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
#include "nodes/pg_list.h"

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
/*
 * Per-call transform context: the function's language and its declared
 * TRANSFORM FOR TYPE types.  Passed into the tuple builders so that a column
 * whose type has a transform is converted through it (in trigger rows,
 * composites and set-returning results), in both directions.  A NULL context,
 * or NIL trftypes, means "no transforms" -- e.g. rows read back from SPI.
 */
typedef struct plphp_trans_ctx
{
	Oid			lang_oid;
	List	   *trftypes;
} plphp_trans_ctx;

/* Resolve a column type's From/To SQL transform under a context, or 0. */
Oid plphp_col_transform(plphp_trans_ctx *tctx, Oid typid, bool fromsql);

zval *plphp_zval_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
HeapTuple plphp_htup_from_zval(zval *val, TupleDesc tupdesc,
						 plphp_trans_ctx *tctx);
HeapTuple plphp_srf_htup_from_zval(zval *val, AttInMetadata *attinmeta,
						 MemoryContext cxt, plphp_trans_ctx *tctx);
char *plphp_convert_to_pg_array(zval *array);
char *plphp_convert_to_pg_array_typed(zval *array, Oid elemtype);
zval *plphp_convert_from_pg_array(char *input, Oid elemtype);
zval *plphp_convert_from_pg_record(const char *input, TupleDesc tupdesc);
char *plphp_zval_get_typed_cstring(zval *val, Oid typid);
zval *plphp_array_get_elem(zval* array, char *key);
char *plphp_zval_get_cstring(zval *val, bool do_array, bool null_ok);
zval *plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc,
						 plphp_trans_ctx *tctx);
HeapTuple plphp_modify_tuple(zval *outdata, TriggerData *tdata,
						 plphp_trans_ctx *tctx);


#endif /* PLPHP_IO_H */
