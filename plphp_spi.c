/**********************************************************************
 * plphp_spi.c - SPI-related functions for PL/php.
 *
 * This software is copyright (c) Command Prompt Inc.
 *
 * The author hereby grants permission to use, copy, modify,
 * distribute, and license this software and its documentation for any
 * purpose, provided that existing copyright notices are retained in
 * all copies and that this notice is included verbatim in any
 * distributions. No written agreement, license, or royalty fee is
 * required for any of the authorized uses.  Modifications to this
 * software may be copyrighted by their author and need not follow the
 * licensing terms described here, provided that the new terms are
 * clearly indicated on the first page of each file where they apply.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY
 * DERIVATIVES THEREOF, EVEN IF THE AUTHOR HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 * NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS,
 * AND THE AUTHOR AND DISTRIBUTORS HAVE NO OBLIGATION TO PROVIDE
 * MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * IDENTIFICATION
 *		$Id$
 *********************************************************************
 */

#include "postgres.h"
#include "plphp_spi.h"
#include "plphp_io.h"

/* PHP stuff */
#include "php.h"

/* PostgreSQL stuff */
#include "access/htup_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#undef DEBUG_PLPHP_MEMORY

#ifdef DEBUG_PLPHP_MEMORY
#define REPORT_PHP_MEMUSAGE(where) \
	elog(NOTICE, "PHP mem usage: %s: %zu", where, zend_memory_usage(0));
#else
#define REPORT_PHP_MEMUSAGE(a)
#endif

/* resource type Id for SPIresult */
int SPIres_rtype;

/*
 * Argument metadata for the PHP-callable functions.  PHP 8 requires internal
 * functions to carry arg_info, so we provide minimal, permissive descriptors.
 */
ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_result, 0, 0, 1)
	ZEND_ARG_INFO(0, result)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pg_raise, 0, 0, 2)
	ZEND_ARG_INFO(0, level)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_return_next, 0, 0, 0)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

/* SPI function table */
zend_function_entry spi_functions[] =
{
	ZEND_FE(spi_exec, arginfo_spi_exec)
	ZEND_FE(spi_fetch_row, arginfo_spi_result)
	ZEND_FE(spi_processed, arginfo_spi_result)
	ZEND_FE(spi_status, arginfo_spi_result)
	ZEND_FE(spi_rewind, arginfo_spi_result)
	ZEND_FE(pg_raise, arginfo_pg_raise)
	ZEND_FE(return_next, arginfo_return_next)
	ZEND_FE_END
};

/* SRF support: */
FunctionCallInfo current_fcinfo = NULL;
TupleDesc current_tupledesc = NULL;
AttInMetadata *current_attinmeta = NULL;
MemoryContext current_memcxt = NULL;
Tuplestorestate *current_tuplestore = NULL;


/* A symbol table to save for return_next for the RETURNS TABLE case */
HashTable *saved_symbol_table;

static zval *get_table_arguments(AttInMetadata *attinmeta);

/*
 * spi_exec
 * 		PL/php equivalent to SPI_exec().
 *
 * This function creates and return a PHP resource which describes the result
 * of a user-specified query.  If the query returns tuples, it's possible to
 * retrieve them by using spi_fetch_row.
 *
 * Receives one or two arguments.  The mandatory first argument is the query
 * text.  The optional second argument is the tuple limit.
 *
 * Note that just like PL/Perl, we start a subtransaction before invoking the
 * SPI call, and automatically roll it back if the call fails.
 */
ZEND_FUNCTION(spi_exec)
{
	char	   *query;
	size_t		query_len;
	long		status;
	zend_long	limit = 0;
	php_SPIresult *SPIres;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	REPORT_PHP_MEMUSAGE("spi_exec called");

	/* Parse arguments */
	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|l",
							  &query, &query_len, &limit) == FAILURE)
	{
		zend_error(E_WARNING, "Can not parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	/* Call SPI */
	PG_TRY();
	{
		status = SPI_exec(query, limit);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData	*edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/* bail PHP out */
		zend_error(E_ERROR, "%s", edata->message);

		/* Can't get here, but keep compiler quiet */
		return;
	}
	PG_END_TRY();

	/* This malloc'ed chunk is freed in php_SPIresult_destroy */
	SPIres = (php_SPIresult *) malloc(sizeof(php_SPIresult));
	if (!SPIres)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Prepare the return resource */
	SPIres->SPI_processed = SPI_processed;
	if (status == SPI_OK_SELECT)
		SPIres->SPI_tuptable = SPI_tuptable;
	else
		SPIres->SPI_tuptable = NULL;
	SPIres->current_row = 0;
	SPIres->status = status;

	REPORT_PHP_MEMUSAGE("spi_exec: creating resource");

	/* Register the resource to PHP so it will be able to free it */
	RETURN_RES(zend_register_resource((void *) SPIres, SPIres_rtype));
}

/*
 * spi_fetch_row
 * 		Grab a row from a SPI result (from spi_exec).
 *
 * This function receives a resource Id and returns a PHP hash representing the
 * next tuple in the result, or false if no tuples remain.
 */
ZEND_FUNCTION(spi_fetch_row)
{
	zval	   *z_spi = NULL;
	zval	   *row;
	php_SPIresult	*SPIres;

	REPORT_PHP_MEMUSAGE("spi_fetch_row: called");

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Can not parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	SPIres = (php_SPIresult *) zend_fetch_resource(Z_RES_P(z_spi),
												   "SPI result", SPIres_rtype);
	if (SPIres == NULL)
		RETURN_FALSE;

	if (SPIres->status != SPI_OK_SELECT)
	{
		zend_error(E_WARNING, "SPI status is not good");
		RETURN_FALSE;
	}

	if (SPIres->current_row < SPIres->SPI_processed)
	{
		row = plphp_zval_from_tuple(SPIres->SPI_tuptable->vals[SPIres->current_row],
									SPIres->SPI_tuptable->tupdesc);
		SPIres->current_row++;

		/* Move the freshly built array into return_value and free the shell */
		ZVAL_COPY_VALUE(return_value, row);
		efree(row);
	}
	else
		RETURN_FALSE;

	REPORT_PHP_MEMUSAGE("spi_fetch_row: finish");
}

/*
 * spi_processed
 * 		Return the number of tuples returned in a spi_exec call.
 */
ZEND_FUNCTION(spi_processed)
{
	zval	   *z_spi = NULL;
	php_SPIresult	*SPIres;

	REPORT_PHP_MEMUSAGE("spi_processed: start");

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	SPIres = (php_SPIresult *) zend_fetch_resource(Z_RES_P(z_spi),
												   "SPI result", SPIres_rtype);
	if (SPIres == NULL)
		RETURN_FALSE;

	REPORT_PHP_MEMUSAGE("spi_processed: finish");

	RETURN_LONG((zend_long) SPIres->SPI_processed);
}

/*
 * spi_status
 * 		Return the status returned by a previous spi_exec call, as a string.
 */
ZEND_FUNCTION(spi_status)
{
	zval	   *z_spi = NULL;
	php_SPIresult	*SPIres;

	REPORT_PHP_MEMUSAGE("spi_status: start");

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	SPIres = (php_SPIresult *) zend_fetch_resource(Z_RES_P(z_spi),
												   "SPI result", SPIres_rtype);
	if (SPIres == NULL)
		RETURN_FALSE;

	REPORT_PHP_MEMUSAGE("spi_status: finish");

	/* RETURN_STRING copies the string in PHP 7+ */
	RETURN_STRING(SPI_result_code_string(SPIres->status));
}

/*
 * spi_rewind
 * 		Resets the internal counter for spi_fetch_row, so the next
 * 		spi_fetch_row call will start fetching from the beginning.
 */
ZEND_FUNCTION(spi_rewind)
{
	zval	   *z_spi = NULL;
	php_SPIresult	*SPIres;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	SPIres = (php_SPIresult *) zend_fetch_resource(Z_RES_P(z_spi),
												   "SPI result", SPIres_rtype);
	if (SPIres == NULL)
		RETURN_FALSE;

	SPIres->current_row = 0;

	RETURN_NULL();
}
/*
 * pg_raise
 *      User-callable function for sending messages to the Postgres log.
 */
ZEND_FUNCTION(pg_raise)
{
	char       *level = NULL,
			   *message = NULL;
	size_t      level_len,
				message_len;
	int			elevel = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss",
							  &level, &level_len,
							  &message, &message_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		return;
	}

	if (strcasecmp(level, "ERROR") == 0)
		elevel = E_ERROR;
	else if (strcasecmp(level, "WARNING") == 0)
		elevel = E_WARNING;
	else if (strcasecmp(level, "NOTICE") == 0)
		elevel = E_NOTICE;
	else
		zend_error(E_ERROR, "incorrect log level");

	zend_error(elevel, "%s", message);
}

/*
 * return_next
 * 		Add a tuple to the current tuplestore
 */
ZEND_FUNCTION(return_next)
{
	MemoryContext	oldcxt;
	zval	   *param = NULL;
	bool		free_param = false;
	HeapTuple	tup;
	ReturnSetInfo *rsi;

	/*
	 * Disallow use of return_next inside non-SRF functions
	 */
	if (current_fcinfo == NULL || current_fcinfo->flinfo == NULL ||
		!current_fcinfo->flinfo->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use return_next in functions not declared to "
						"return a set")));

	rsi = (ReturnSetInfo *) current_fcinfo->resultinfo;

	Assert(current_tupledesc != NULL);
	Assert(rsi != NULL);

	if (ZEND_NUM_ARGS() > 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("wrong number of arguments to %s", "return_next")));

	if (ZEND_NUM_ARGS() == 0)
	{
		/*
		 * Called from the function declared with RETURNS TABLE.  Grab the
		 * calling PL/php function's symbol table so we can read the OUT/TABLE
		 * argument variables ($colname) out of it.
		 */
		saved_symbol_table = zend_rebuild_symbol_table();
		param = get_table_arguments(current_attinmeta);
		free_param = true;
	}
	else if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &param) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		return;
	}

	/* Use the per-query context so that the tuplestore survives */
	oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	/* Form the tuple */
	tup = plphp_srf_htup_from_zval(param, current_attinmeta, current_memcxt);

	/* First call?  Create the tuplestore. */
	if (!current_tuplestore)
		current_tuplestore = tuplestore_begin_heap(true, false, work_mem);

	/* Save the tuple and clean up */
	tuplestore_puttuple(current_tuplestore, tup);
	heap_freetuple(tup);

	MemoryContextSwitchTo(oldcxt);

	/* Free the array we built for the RETURNS TABLE case */
	if (free_param)
	{
		zval_ptr_dtor(param);
		efree(param);
	}
}

/*
 * php_SPIresult_destroy
 * 		Free the resources allocated by a spi_exec call.
 *
 * This is automatically called when the resource goes out of scope
 * or is overwritten by another resource.
 */
void
php_SPIresult_destroy(zend_resource *rsrc)
{
	php_SPIresult *res = (php_SPIresult *) rsrc->ptr;

	if (res->SPI_tuptable != NULL)
		SPI_freetuptable(res->SPI_tuptable);

	free(res);
}

/* Return an array of TABLE argument values for return_next */
static
zval *get_table_arguments(AttInMetadata *attinmeta)
{
	zval   *retval;
	int		i;

	retval = (zval *) emalloc(sizeof(zval));
	array_init(retval);

	Assert(attinmeta->tupdesc);
	Assert(saved_symbol_table != NULL);
	/* Extract OUT argument names */
	for (i = 0; i < attinmeta->tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(attinmeta->tupdesc, i);
		zval   *val;
		char   *attname;

		Assert(!att->attisdropped);

		attname = NameStr(att->attname);

		val = zend_hash_str_find(saved_symbol_table, attname, strlen(attname));
		if (val != NULL)
		{
			/* Symbol-table entries for live locals are indirect CV slots */
			if (Z_TYPE_P(val) == IS_INDIRECT)
				val = Z_INDIRECT_P(val);
			/* The alias variables are references into $args; resolve them */
			ZVAL_DEREF(val);
			if (Z_TYPE_P(val) == IS_UNDEF)
				add_next_index_null(retval);
			else
			{
				/* borrowed from the symbol table -- add a reference */
				Z_TRY_ADDREF_P(val);
				add_next_index_zval(retval, val);
			}
		}
		else
			add_next_index_null(retval);
	}
	return retval;
}


/*
 * vim:ts=4:sw=4:cino=(0
 */
