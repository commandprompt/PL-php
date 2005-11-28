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

#include "plphp_spi.h"
#include "plphp_io.h"

/* PHP stuff */
#include "php.h"

/* resource type Id for SPIresult */
int SPIres_rtype;

/* SPI function table */
zend_function_entry spi_functions[] =
{
	ZEND_FE(spi_exec, NULL)
	ZEND_FE(spi_fetch_row, NULL)
	ZEND_FE(spi_processed, NULL)
	ZEND_FE(spi_status, NULL)
	ZEND_FE(spi_rewind, NULL)
	ZEND_FE(pg_raise, NULL)
	{NULL, NULL, NULL}
};

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
 */
ZEND_FUNCTION(spi_exec)
{
	char	   *query;
	int			query_len;
	long		status;
	long		limit;
	php_SPIresult *SPIres;
	int			spi_id;
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;

	if ((ZEND_NUM_ARGS() > 2) || (ZEND_NUM_ARGS() < 1))
		WRONG_PARAM_COUNT;

	/* Parse arguments */
	if (ZEND_NUM_ARGS() == 2)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl",
								  &query, &query_len, &limit) == FAILURE)
		{
			zend_error(E_WARNING, "Can not parse parameters in %s",
					   get_active_function_name(TSRMLS_C));
			RETURN_FALSE;
		}
	}
	else if (ZEND_NUM_ARGS() == 1)
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",
								  &query, &query_len) == FAILURE)
		{
			zend_error(E_WARNING, "Can not parse parameters in %s",
					   get_active_function_name(TSRMLS_C));
			RETURN_FALSE;
		}
		limit = 0;
	}
	else
	{
		zend_error(E_WARNING, "Incorrect number of parameters to %s",
				   get_active_function_name(TSRMLS_C));
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

		/*
		 * AtEOSubXact_SPI() should not have popped any SPI context, but just
		 * in case it did, make sure we remain connected.
		 */
		SPI_restore_connection();
	}
	PG_CATCH();
	{
		ErrorData	*edata;

		/* Save error info */
		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/* Abort the inner trasaction */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		/*
		 * If AtEOSubXact_SPI() popped any SPI context of the subxact, it will
		 * have left us in a disconnected state.  We need this hack to return
		 * to connected state.
		 */
		SPI_restore_connection();

		/* bail PHP out */
		zend_error(E_ERROR, strdup(edata->message));

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
	SPIres->current_row = 0;
	SPIres->status = status;

	/* Register the resource to PHP so it will be able to free it */
	spi_id = ZEND_REGISTER_RESOURCE(return_value, (void *) SPIres,
					 				SPIres_rtype);

	RETURN_RESOURCE(spi_id);
}

/*
 * spi_fetch_row
 * 		Grab a row from a SPI result (from spi_exec).
 *
 * This function receives a resource Id and returns a PHP hash representing the
 * next tuple in the result, or false if no tuples remain.
 *
 * XXX Apparently this is leaking memory.  How do we tell PHP to free the tuple
 * once the user is done with it?
 */
ZEND_FUNCTION(spi_fetch_row)
{
	zval	   *row = NULL;
	zval	  **z_spi = NULL;
	php_SPIresult	*SPIres;

	if (ZEND_NUM_ARGS() != 1)
		WRONG_PARAM_COUNT;

	if (zend_get_parameters_ex(1, &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Can not parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (z_spi == NULL)
	{
		zend_error(E_WARNING, "Could not get SPI resource in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(SPIres, php_SPIresult *, z_spi, -1, "SPI result",
						SPIres_rtype);

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

		*return_value = *row;

		zval_copy_ctor(return_value);
	}
	else
		RETURN_FALSE;
}

ZEND_FUNCTION(spi_processed)
{
	zval	   **z_spi = NULL;
	php_SPIresult	*SPIres;

	if (ZEND_NUM_ARGS() != 1)
		WRONG_PARAM_COUNT;

	if (zend_get_parameters_ex(1, &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (z_spi == NULL)
	{
		zend_error(E_WARNING, "Could not get SPI resource in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(SPIres, php_SPIresult *, z_spi, -1, "SPI result",
						SPIres_rtype);

	RETURN_LONG(SPIres->SPI_processed);
}

ZEND_FUNCTION(spi_status)
{
	zval	   **z_spi = NULL;
	php_SPIresult	*SPIres;

	if (ZEND_NUM_ARGS() != 1)
		WRONG_PARAM_COUNT;

	if (zend_get_parameters_ex(1, &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (z_spi == NULL)
	{
		zend_error(E_WARNING, "Could not get SPI resource in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(SPIres, php_SPIresult *, z_spi, -1, "SPI result",
						SPIres_rtype);

	RETURN_STRING(SPI_result_code_string(SPIres->status), true);
}

ZEND_FUNCTION(spi_rewind)
{
	zval	   **z_spi = NULL;
	php_SPIresult	*SPIres;

	if (ZEND_NUM_ARGS() != 1)
		WRONG_PARAM_COUNT;

	if (zend_get_parameters_ex(1, &z_spi) == FAILURE)
	{
		zend_error(E_WARNING, "Cannot parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}
	if (z_spi == NULL)
	{
		zend_error(E_WARNING, "Could not get SPI resource in %s",
				   get_active_function_name(TSRMLS_C));
		RETURN_FALSE;
	}

	ZEND_FETCH_RESOURCE(SPIres, php_SPIresult *, z_spi, -1, "SPI result",
						SPIres_rtype);

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
	int         level_len,
				message_len,
				elevel = 0;

	if (ZEND_NUM_ARGS() != 2)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("wrong number of arguments to %s", "pg_raise")));
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss",
							  &level, &level_len,
							  &message, &message_len) == FAILURE)
	{
		zend_error(E_WARNING, "cannot parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
	}

	if (strcasecmp(level, "ERROR") == 0)
		elevel = E_ERROR;
	else if (strcasecmp(level, "WARNING") == 0)
		elevel = E_WARNING;
	else if (strcasecmp(level, "NOTICE") == 0)
		elevel = E_NOTICE;
	else
		zend_error(E_ERROR, "incorrect log level");

	zend_error(elevel, message);
}

void
php_SPIresult_destroy(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	php_SPIresult *res = (php_SPIresult *) rsrc->ptr;

	SPI_freetuptable(res->SPI_tuptable);
	free(res);
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
