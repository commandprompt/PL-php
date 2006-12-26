/*
 * $Id$
 */
#ifndef PLPHP_SPI_H
#define PLPHP_SPI_H

#define Debug DestDebug
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
#undef Debug

#include "Zend/zend_API.h"

/* resource type Id for SPIresult */
extern int SPIres_rtype;
/* Function table */
extern zend_function_entry spi_functions[];

/* SRF support: */
extern FunctionCallInfo current_fcinfo;
extern TupleDesc current_tupledesc;
extern AttInMetadata *current_attinmeta;
extern MemoryContext current_memcxt;
extern Tuplestorestate *current_tuplestore;

/*
 * Definition for PHP "resource" result type from SPI_execute.
 */
typedef struct
{
	SPITupleTable  *SPI_tuptable;
	uint32			SPI_processed;
	uint32			current_row;
	int				status;
} php_SPIresult;

ZEND_FUNCTION(spi_exec);
ZEND_FUNCTION(spi_fetch_row);
ZEND_FUNCTION(spi_processed);
ZEND_FUNCTION(spi_status);
ZEND_FUNCTION(spi_rewind);
ZEND_FUNCTION(pg_raise);
ZEND_FUNCTION(return_next);

void php_SPIresult_destroy(zend_rsrc_list_entry *rsrc TSRMLS_DC);

#endif /* PLPHP_SPI_H */

/*
 * vim:ts=4:sw=4:cino=(0
 */
