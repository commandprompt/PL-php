/*
 * $Id$
 */

#ifndef PLPHP_IO_H
#define PLPHP_IO_H

/* PostgreSQL stuff */
#include "postgres.h"
#include "access/heapam.h"

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

zval *plphp_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc);
char *plphp_convert_to_pg_array(zval *array);
zval *plphp_convert_from_pg_array(char *input);
zval *plphp_get_row(zval * array, int index);
char *plphp_get_elem(zval * array, char *key);
int plphp_attr_count(zval *array);
int plphp_get_rows_num(zval *array);
int plphp_attr_count_r(zval *array);
char **plphp_get_attr_name(zval *array);
zval *plphp_get_new(zval *array);
zval *plphp_get_pelem(zval *array, char *key);

#endif /* PLPHP_IO_H */
