/*
 * $Id$
 */
#ifndef PLPHP_SPI_H
#define PLPHP_SPI_H

#include "executor/spi.h"
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

#include "Zend/zend_API.h"

/* resource type Id for SPIresult */
extern int SPIres_rtype;
/* resource type Id for a prepared SPI plan */
extern int SPIplan_rtype;
/* Function table */
extern zend_function_entry spi_functions[];
/* The PgError exception class (registered in plphp_init) */
extern zend_class_entry *plphp_PgError_ce;

/* SRF support: */
extern FunctionCallInfo current_fcinfo;
extern TupleDesc current_tupledesc;
extern AttInMetadata *current_attinmeta;
extern MemoryContext current_memcxt;
extern Tuplestorestate *current_tuplestore;
extern HashTable *saved_symbol_table;

/*
 * Definition for PHP "resource" result type from SPI_execute.
 */
typedef struct
{
	SPITupleTable  *SPI_tuptable;
	uint64			SPI_processed;	/* SPI_processed is uint64 since PG 11 */
	uint64			current_row;
	int				status;
} php_SPIresult;

ZEND_FUNCTION(spi_exec);
ZEND_FUNCTION(spi_fetch_row);
ZEND_FUNCTION(spi_processed);
ZEND_FUNCTION(spi_status);
ZEND_FUNCTION(spi_rewind);
ZEND_FUNCTION(pg_raise);
ZEND_FUNCTION(return_next);
ZEND_FUNCTION(spi_prepare);
ZEND_FUNCTION(spi_exec_prepared);
ZEND_FUNCTION(spi_query_prepared);
ZEND_FUNCTION(spi_query);
ZEND_FUNCTION(spi_fetchrow);
ZEND_FUNCTION(spi_cursor_close);
ZEND_FUNCTION(spi_each);
ZEND_FUNCTION(spi_freeplan);
ZEND_FUNCTION(quote_literal);
ZEND_FUNCTION(quote_nullable);
ZEND_FUNCTION(quote_ident);
ZEND_FUNCTION(elog);
ZEND_FUNCTION(spi_commit);
ZEND_FUNCTION(spi_rollback);
ZEND_FUNCTION(subtransaction);

void php_SPIresult_destroy(zend_resource *rsrc);
void php_SPIplan_destroy(zend_resource *rsrc);
void plphp_throw_pg_error(ErrorData *edata);

#endif /* PLPHP_SPI_H */

/*
 * vim:ts=4:sw=4:cino=(0
 */
