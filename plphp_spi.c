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
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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
/* resource type Id for a prepared SPI plan */
int SPIplan_rtype;

/*
 * A prepared plan, exposed to PHP as a "plphp SPI plan" resource.
 */
typedef struct
{
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *argtypes;		/* malloc'd, length nargs */
	Oid		   *typinput;		/* input function Oids, length nargs */
	Oid		   *typioparam;		/* input typioparams, length nargs */
} php_SPIplan;

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

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_prepare, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_VARIADIC_INFO(0, argtypes)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_exec_prepared, 0, 0, 1)
	ZEND_ARG_INFO(0, plan)
	ZEND_ARG_VARIADIC_INFO(0, args)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_freeplan, 0, 0, 1)
	ZEND_ARG_INFO(0, plan)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_query, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_cursor, 0, 0, 1)
	ZEND_ARG_INFO(0, cursor)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_quote, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_elog, 0, 0, 2)
	ZEND_ARG_INFO(0, level)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_spi_noargs, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_subtransaction, 0, 0, 1)
	ZEND_ARG_INFO(0, callback)
	ZEND_ARG_VARIADIC_INFO(0, args)
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
	ZEND_FE(spi_prepare, arginfo_spi_prepare)
	ZEND_FE(spi_exec_prepared, arginfo_spi_exec_prepared)
	ZEND_FE(spi_query_prepared, arginfo_spi_exec_prepared)
	ZEND_FE(spi_query, arginfo_spi_query)
	ZEND_FE(spi_fetchrow, arginfo_spi_cursor)
	ZEND_FE(spi_cursor_close, arginfo_spi_cursor)
	ZEND_FE(spi_freeplan, arginfo_spi_freeplan)
	ZEND_FE(quote_literal, arginfo_quote)
	ZEND_FE(quote_nullable, arginfo_quote)
	ZEND_FE(quote_ident, arginfo_quote)
	ZEND_FE(elog, arginfo_elog)
	ZEND_FE(spi_commit, arginfo_spi_noargs)
	ZEND_FE(spi_rollback, arginfo_spi_noargs)
	ZEND_FE(subtransaction, arginfo_subtransaction)
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
 * php_SPIplan_destroy
 * 		Free a prepared plan resource (from spi_prepare).
 */
void
php_SPIplan_destroy(zend_resource *rsrc)
{
	php_SPIplan *plan = (php_SPIplan *) rsrc->ptr;

	if (plan == NULL)
		return;
	if (plan->plan != NULL)
		SPI_freeplan(plan->plan);
	if (plan->argtypes)
		free(plan->argtypes);
	if (plan->typinput)
		free(plan->typinput);
	if (plan->typioparam)
		free(plan->typioparam);
	free(plan);
}

/*
 * spi_prepare
 * 		Prepare a query plan.  The first argument is the query text; any
 * 		further arguments are the SQL type names of the query's $1, $2, ...
 * 		placeholders.  Returns a plan resource for use with
 * 		spi_exec_prepared() or spi_query_prepared(); free it with
 * 		spi_freeplan().
 */
ZEND_FUNCTION(spi_prepare)
{
	char	   *query;
	size_t		query_len;
	zval	   *ztypes = NULL;
	int			ntypes = 0;
	php_SPIplan *plan;
	SPIPlanPtr	spiplan;
	Oid		   *argtypes = NULL;
	Oid		   *typinput = NULL;
	Oid		   *typioparam = NULL;
	int			i;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s*", &query, &query_len,
							  &ztypes, &ntypes) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	if (ntypes > 0)
	{
		argtypes = (Oid *) malloc(ntypes * sizeof(Oid));
		typinput = (Oid *) malloc(ntypes * sizeof(Oid));
		typioparam = (Oid *) malloc(ntypes * sizeof(Oid));
	}

	PG_TRY();
	{
		for (i = 0; i < ntypes; i++)
		{
			char   *typ = plphp_zval_get_cstring(&ztypes[i], false, false);
			Oid		typid;
			int32	typmod;

			parseTypeString(typ, &typid, &typmod, NULL);
			argtypes[i] = typid;
			getTypeInputInfo(typid, &typinput[i], &typioparam[i]);
			pfree(typ);
		}

		spiplan = SPI_prepare(query, ntypes, argtypes);
		if (spiplan == NULL)
			elog(ERROR, "spi_prepare: SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));

		/* Make the plan outlive the current SPI context */
		if (SPI_keepplan(spiplan) != 0)
			elog(ERROR, "spi_prepare: SPI_keepplan failed");
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		CurrentResourceOwner = oldowner;
		if (argtypes)
			free(argtypes);
		if (typinput)
			free(typinput);
		if (typioparam)
			free(typioparam);
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	/* This chunk is freed in php_SPIplan_destroy */
	plan = (php_SPIplan *) malloc(sizeof(php_SPIplan));
	if (!plan)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
	plan->plan = spiplan;
	plan->nargs = ntypes;
	plan->argtypes = argtypes;
	plan->typinput = typinput;
	plan->typioparam = typioparam;

	RETURN_RES(zend_register_resource((void *) plan, SPIplan_rtype));
}

/*
 * spi_exec_prepared
 * 		Execute a plan prepared with spi_prepare(), passing the plan resource
 * 		followed by one argument per placeholder.  Returns a result resource
 * 		usable with spi_fetch_row()/spi_processed()/spi_status(), exactly like
 * 		spi_exec().
 */
ZEND_FUNCTION(spi_exec_prepared)
{
	zval	   *z_plan;
	zval	   *zargs = NULL;
	int			nargs = 0;
	php_SPIplan *plan;
	php_SPIresult *SPIres;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	int			i;
	long		status;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r*", &z_plan,
							  &zargs, &nargs) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	plan = (php_SPIplan *) zend_fetch_resource(Z_RES_P(z_plan),
											   "plphp SPI plan", SPIplan_rtype);
	if (plan == NULL)
		RETURN_FALSE;

	if (nargs != plan->nargs)
	{
		zend_error(E_WARNING,
				   "spi_exec_prepared: plan expects %d argument(s), got %d",
				   plan->nargs, nargs);
		RETURN_FALSE;
	}

	if (plan->nargs > 0)
	{
		values = (Datum *) palloc(plan->nargs * sizeof(Datum));
		nulls = (char *) palloc(plan->nargs * sizeof(char));
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		for (i = 0; i < plan->nargs; i++)
		{
			char   *val = plphp_zval_get_cstring(&zargs[i], true, true);

			if (val == NULL)
			{
				nulls[i] = 'n';
				values[i] = (Datum) 0;
			}
			else
			{
				nulls[i] = ' ';
				values[i] = OidInputFunctionCall(plan->typinput[i], val,
												 plan->typioparam[i], -1);
				pfree(val);
			}
		}

		status = SPI_execute_plan(plan->plan, values, nulls, false, 0);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	/* This chunk is freed in php_SPIresult_destroy */
	SPIres = (php_SPIresult *) malloc(sizeof(php_SPIresult));
	if (!SPIres)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
	SPIres->SPI_processed = SPI_processed;
	if (status == SPI_OK_SELECT)
		SPIres->SPI_tuptable = SPI_tuptable;
	else
		SPIres->SPI_tuptable = NULL;
	SPIres->current_row = 0;
	SPIres->status = status;

	RETURN_RES(zend_register_resource((void *) SPIres, SPIres_rtype));
}

/*
 * spi_query
 * 		Open a cursor for a query and return its portal name.
 *
 * Unlike spi_exec, this does not materialize the result set: rows are
 * fetched one at a time with spi_fetchrow(), so arbitrarily large results
 * can be scanned in constant memory.  The cursor is closed automatically
 * when spi_fetchrow() reaches the end, or early with spi_cursor_close();
 * any cursor still open is destroyed at transaction end.
 */
ZEND_FUNCTION(spi_query)
{
	char	   *query;
	size_t		query_len;
	Portal		portal;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s",
							  &query, &query_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		SPIPlanPtr	plan;

		plan = SPI_prepare(query, 0, NULL);
		if (plan == NULL)
			elog(ERROR, "spi_query: SPI_prepare failed: %s",
				 SPI_result_code_string(SPI_result));

		/* the portal keeps its own copy of the plan */
		portal = SPI_cursor_open(NULL, plan, NULL, NULL, false);
		if (portal == NULL)
			elog(ERROR, "spi_query: SPI_cursor_open failed");
		SPI_freeplan(plan);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	RETURN_STRING(portal->name);
}

/*
 * spi_query_prepared
 * 		Open a cursor for a plan prepared with spi_prepare(), passing one
 * 		argument per placeholder, and return its portal name for use with
 * 		spi_fetchrow().  The streaming counterpart of spi_exec_prepared().
 */
ZEND_FUNCTION(spi_query_prepared)
{
	zval	   *z_plan;
	zval	   *zargs = NULL;
	int			nargs = 0;
	php_SPIplan *plan;
	Portal		portal;
	Datum	   *values = NULL;
	char	   *nulls = NULL;
	int			i;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r*", &z_plan,
							  &zargs, &nargs) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	plan = (php_SPIplan *) zend_fetch_resource(Z_RES_P(z_plan),
											   "plphp SPI plan", SPIplan_rtype);
	if (plan == NULL)
		RETURN_FALSE;

	if (nargs != plan->nargs)
	{
		zend_error(E_WARNING,
				   "spi_query_prepared: plan expects %d argument(s), got %d",
				   plan->nargs, nargs);
		RETURN_FALSE;
	}

	if (plan->nargs > 0)
	{
		values = (Datum *) palloc(plan->nargs * sizeof(Datum));
		nulls = (char *) palloc(plan->nargs * sizeof(char));
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		for (i = 0; i < plan->nargs; i++)
		{
			char   *val = plphp_zval_get_cstring(&zargs[i], true, true);

			if (val == NULL)
			{
				nulls[i] = 'n';
				values[i] = (Datum) 0;
			}
			else
			{
				nulls[i] = ' ';
				values[i] = OidInputFunctionCall(plan->typinput[i], val,
												 plan->typioparam[i], -1);
				pfree(val);
			}
		}

		portal = SPI_cursor_open(NULL, plan->plan, values, nulls, false);
		if (portal == NULL)
			elog(ERROR, "spi_query_prepared: SPI_cursor_open failed");

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	RETURN_STRING(portal->name);
}

/*
 * spi_fetchrow
 * 		Fetch the next row from a cursor opened with spi_query() or
 * 		spi_query_prepared(), as an associative array keyed by column name.
 *
 * Returns false when the cursor is exhausted (closing it automatically,
 * like PL/Perl) or when no cursor by that name exists.
 */
ZEND_FUNCTION(spi_fetchrow)
{
	char	   *cursor;
	size_t		cursor_len;
	zval	   *row = NULL;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s",
							  &cursor, &cursor_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		Portal		portal = SPI_cursor_find(cursor);

		if (portal != NULL)
		{
			SPI_cursor_fetch(portal, true, 1);
			if (SPI_processed == 0)
				SPI_cursor_close(portal);
			else
				row = plphp_zval_from_tuple(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc);
			SPI_freetuptable(SPI_tuptable);
		}

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	if (row == NULL)
		RETURN_FALSE;

	/* Move the freshly built array into return_value and free the shell */
	ZVAL_COPY_VALUE(return_value, row);
	efree(row);
}

/*
 * spi_cursor_close
 * 		Close a cursor opened with spi_query() or spi_query_prepared()
 * 		before it is exhausted.  Closing an unknown or already-closed
 * 		cursor is silently ignored, as in PL/Perl.
 */
ZEND_FUNCTION(spi_cursor_close)
{
	char	   *cursor;
	size_t		cursor_len;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s",
							  &cursor, &cursor_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		Portal		portal = SPI_cursor_find(cursor);

		if (portal != NULL)
			SPI_cursor_close(portal);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();

	RETURN_TRUE;
}

/*
 * spi_freeplan
 * 		Release a plan created by spi_prepare().
 */
ZEND_FUNCTION(spi_freeplan)
{
	zval	   *z_plan;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r", &z_plan) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	if (zend_fetch_resource(Z_RES_P(z_plan), "plphp SPI plan",
							SPIplan_rtype) == NULL)
		RETURN_FALSE;

	/* Closing the resource runs php_SPIplan_destroy */
	zend_list_close(Z_RES_P(z_plan));
	RETURN_TRUE;
}

/*
 * quote_literal
 * 		Return the argument suitably quoted for use as a string literal in an
 * 		SQL statement.
 */
ZEND_FUNCTION(quote_literal)
{
	char	   *str;
	size_t		len;
	char	   *quoted;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	quoted = quote_literal_cstr(str);
	RETVAL_STRING(quoted);
	pfree(quoted);
}

/*
 * quote_nullable
 * 		Like quote_literal, but a PHP null yields the SQL string "NULL".
 */
ZEND_FUNCTION(quote_nullable)
{
	char	   *str = NULL;
	size_t		len = 0;
	char	   *quoted;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s!", &str, &len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	if (str == NULL)
		RETURN_STRING("NULL");

	quoted = quote_literal_cstr(str);
	RETVAL_STRING(quoted);
	pfree(quoted);
}

/*
 * quote_ident
 * 		Return the argument suitably quoted for use as an SQL identifier.
 */
ZEND_FUNCTION(quote_ident)
{
	char	   *str;
	size_t		len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &str, &len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	/* quote_identifier may return its argument unchanged, so don't free it */
	RETURN_STRING(quote_identifier(str));
}

/*
 * elog
 * 		PL/Perl-compatible logging: elog(level, message) where level is one of
 * 		DEBUG, LOG, INFO, NOTICE, WARNING or ERROR.
 */
ZEND_FUNCTION(elog)
{
	char	   *level = NULL,
			   *message = NULL;
	size_t		level_len,
				message_len;
	int			elevel;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss",
							  &level, &level_len,
							  &message, &message_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		return;
	}

	if (strcasecmp(level, "DEBUG") == 0)
		elevel = DEBUG1;
	else if (strcasecmp(level, "LOG") == 0)
		elevel = LOG;
	else if (strcasecmp(level, "INFO") == 0)
		elevel = INFO;
	else if (strcasecmp(level, "NOTICE") == 0)
		elevel = NOTICE;
	else if (strcasecmp(level, "WARNING") == 0)
		elevel = WARNING;
	else if (strcasecmp(level, "ERROR") == 0)
		elevel = ERROR;
	else
	{
		zend_error(E_ERROR, "elog: unrecognized level \"%s\"", level);
		return;
	}

	if (elevel == ERROR)
		/* route through the PHP error path so it unwinds cleanly */
		zend_error(E_ERROR, "%s", message);
	else
		ereport(elevel, (errmsg("%s", message)));
}

/*
 * spi_commit
 * 		Commit the current transaction and start a new one.  Only valid inside
 * 		a procedure invoked by CALL in a non-atomic context (SPI_commit itself
 * 		enforces this and raises an error otherwise).
 */
ZEND_FUNCTION(spi_commit)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_commit();
#if PG_VERSION_NUM < 150000
		/* Before PG 15, SPI_commit did not start a new transaction */
		SPI_start_transaction();
#endif
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();
}

/*
 * spi_rollback
 * 		Roll back the current transaction and start a new one.  Like
 * 		spi_commit, only valid in a non-atomic procedure call.
 */
ZEND_FUNCTION(spi_rollback)
{
	MemoryContext oldcontext = CurrentMemoryContext;

	PG_TRY();
	{
		SPI_rollback();
#if PG_VERSION_NUM < 150000
		/* Before PG 15, SPI_rollback did not start a new transaction */
		SPI_start_transaction();
#endif
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		zend_error(E_ERROR, "%s", edata->message);
		return;
	}
	PG_END_TRY();
}

/*
 * subtransaction
 * 		Run a PHP callable inside an internal subtransaction.  If the callable
 * 		returns normally the subtransaction is committed and its return value is
 * 		passed back; if it raises an error -- a PHP exception or a database
 * 		error -- the subtransaction is rolled back and the error re-raised.
 *
 *		subtransaction(callable [, arg, ...])
 */
ZEND_FUNCTION(subtransaction)
{
	zend_fcall_info			fci;
	zend_fcall_info_cache	fcc;
	zval				   *fargs = NULL;
	int						fargc = 0;
	zval					result;
	MemoryContext			oldcontext = CurrentMemoryContext;
	ResourceOwner			oldowner = CurrentResourceOwner;
	volatile bool			bailed = false;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "f*", &fci, &fcc,
							  &fargs, &fargc) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name());
		RETURN_FALSE;
	}

	ZVAL_UNDEF(&result);

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Run the callback.  In PL/php a database error surfaces as a Zend bailout
	 * (zend_error(E_ERROR) from our SPI wrappers), so we catch that here; a
	 * plain PHP "throw" instead leaves a pending exception.  Both mean the body
	 * failed and the subtransaction must be rolled back.
	 */
	zend_try
	{
		fci.retval = &result;
		fci.params = fargs;
		fci.param_count = fargc;

		if (zend_call_function(&fci, &fcc) == FAILURE)
			zend_error(E_ERROR, "subtransaction: could not call the callback");
	}
	zend_catch
	{
		bailed = true;
	}
	zend_end_try();

	if (bailed || EG(exception) != NULL)
	{
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		if (bailed)
			zend_bailout();		/* re-propagate the database error */
		return;					/* otherwise a PHP exception is pending */
	}

	/* Success: commit the subtransaction and hand back the body's value */
	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcontext);
	CurrentResourceOwner = oldowner;

	if (Z_TYPE(result) == IS_UNDEF)
		RETURN_NULL();
	ZVAL_COPY_VALUE(return_value, &result);
}


/*
 * vim:ts=4:sw=4:cino=(0
 */
