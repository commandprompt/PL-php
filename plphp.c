/**********************************************************************
 * plphp.c - PHP as a procedural language for PostgreSQL
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
 *
 * TODO
 *
 * 1. Check all palloc() and malloc() calls: where are they freed,
 * and what MemoryContext management should we be doing?
 *
 * 2. Add a non-stub validator function.
 *
 * 3. Change all string manipulation to StringInfo.
 *
 *********************************************************************
 */

/* PostgreSQL stuff */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"			/* needed for SRF support */
#include "utils/builtins.h"		/* needed for define oidout */
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

/* PHP stuff */
#include "php.h"

#include "php_variables.h"
#include "php_globals.h"
#include "zend_hash.h"
#include "zend_modules.h"

#include "php_ini.h"			/* needed for INI_HARDCODED */
#include "php_main.h"

/* system stuff */
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#define INI_HARDCODED(name,value) \
		zend_alter_ini_entry(name, sizeof(name), value, strlen(value), \
							 PHP_INI_SYSTEM, PHP_INI_STAGE_ACTIVATE);

/*
 * These symbols are needed by the Apache module (libphp*.so).  We don't
 * actually use those, but they are needed so that the linker can do its
 * job without complaining.
 *
 * FIXME -- this looks like a mighty hacky way of doing things.
*/
void	   *ap_loaded_modules = NULL;
void	   *unixd_config = NULL;
void	   *ap_server_root = NULL;
void	   *apr_pool_cleanup_null = NULL;
void	   *ap_auth_type = NULL;
void	   *ap_log_rerror = NULL;
void	   *ap_hook_post_config = NULL;
void	   *apr_table_add = NULL;
void	   *ap_get_brigade = NULL;
void	   *ap_hook_handler = NULL;
void	   *ap_update_mtime = NULL;
void	   *apr_brigade_flatten = NULL;
void	   *ap_add_cgi_vars = NULL;
void	   *ap_server_root_relative = NULL;
void	   *apr_table_set = NULL;
void	   *ap_set_content_type = NULL;
void	   *ap_get_server_version = NULL;
void	   *apr_pool_cleanup_register = NULL;
void	   *ap_mpm_query = NULL;
void	   *ap_destroy_sub_req = NULL;
void	   *ap_pass_brigade = NULL;
void	   *apr_pstrdup = NULL;
void	   *apr_table_unset = NULL;
void	   *ap_log_error = NULL;
void	   *apr_table_get = NULL;
void	   *ap_sub_req_lookup_uri = NULL;
void	   *apr_psprintf = NULL;
void	   *ap_run_sub_req = NULL;
void	   *apr_palloc = NULL;
void	   *apr_brigade_cleanup = NULL;
void	   *ap_hook_pre_config = NULL;
void	   *ap_rwrite = NULL;
void	   *apr_table_elts = NULL;
void	   *ap_add_version_component = NULL;
void	   *apr_bucket_eos_create = NULL;
void	   *apr_pool_userdata_set = NULL;
void	   *apr_brigade_create = NULL;
void	   *ap_rflush = NULL;
void	   *ap_set_last_modified = NULL;
void	   *ap_add_common_vars = NULL;
void	   *apr_pool_userdata_get = NULL;

/*
 * Return types.  Why on earth is this a bitmask?  Beats me.
 * We should have separate flags instead.
 */
typedef enum pl_type
{
	PL_TUPLE = 1 << 0,
	PL_SET = 1 << 1,
	PL_ARRAY = 1 << 2,
	PL_PSEUDO = 1 << 3
} pl_type;

/*
 * The information we cache about loaded procedures
 */
typedef struct plphp_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		lanpltrusted;
	pl_type		ret_type;
	Oid			ret_oid;		/* Oid of returning type */
	FmgrInfo	result_in_func;
	Oid			result_typioparam;
	int			nargs;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_typioparam[FUNC_MAX_ARGS];
	bool		arg_is_rowtype[FUNC_MAX_ARGS];
	bool		arg_is_p[FUNC_MAX_ARGS];
} plphp_proc_desc;

/*
 * Global data
 */
static bool plphp_first_call = true;
static zval *plphp_proc_array = NULL;
static zval *srf_phpret = NULL;			/* keep returned value */

/*
 * Forward declarations
 */
static void plphp_init_all(void);
void		plphp_init(void);

PG_FUNCTION_INFO_V1(plphp_call_handler);
Datum plphp_call_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plphp_validator);
Datum plphp_validator(PG_FUNCTION_ARGS);

static Datum plphp_func_handler(PG_FUNCTION_ARGS);
static Datum plphp_trigger_handler(PG_FUNCTION_ARGS);

static plphp_proc_desc *compile_plphp_function(Oid, int);
static zval *plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
static zval *plphp_call_php_func(plphp_proc_desc *, FunctionCallInfo);
static zval *plphp_call_php_trig(plphp_proc_desc *, FunctionCallInfo, zval *);

static Datum plphp_srf_handler(PG_FUNCTION_ARGS, plphp_proc_desc *);
static int	plphp_get_rows_num(zval *);
static zval *plphp_get_row(zval *, int);
zval	   *plphp_hash_from_tuple(HeapTuple, TupleDesc);

ZEND_FUNCTION(spi_exec_query);
ZEND_FUNCTION(spi_fetch_row);

/*
 * This routine is a crock, and so is everyplace that calls it.  The problem
 * is that the cached form of plphp functions/queries is allocated permanently
 * (mostly via malloc()) and never released until backend exit.  Subsidiary
 * data structures such as fmgr info records therefore must live forever
 * as well.  A better implementation would store all this stuff in a per-
 * function memory context that could be reclaimed at need.  In the meantime,
 * fmgr_info_cxt must be called specifying TopMemoryContext so that whatever
 * it might allocate, and whatever the eventual function might allocate using
 * fn_mcxt, will live forever too.
 */

static void
perm_fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt(functionId, finfo, TopMemoryContext);
}

/* need for plphp module work */
static void
sapi_plphp_send_header(sapi_header_struct* sapi_header,
					   void *server_context TSRMLS_DC)		
{

}

static inline size_t
sapi_cli_single_write(const char *str, uint str_length)
{
	long		ret;

	ret = write(STDOUT_FILENO, str, str_length);
	if (ret <= 0)
	{
		return 0;
	}
	return ret;
}

static int
sapi_plphp_ub_write(const char *str, uint str_length TSRMLS_DC)
{
	const char *ptr = str;
	uint		remaining = str_length;
	size_t		ret;

	while (remaining > 0)
	{
		ret = sapi_cli_single_write(ptr, remaining);
		if (!ret)
		{
			php_handle_aborted_connection();
		}
		ptr += ret;
		remaining -= ret;
	}

	return str_length;
}

/* php_module startup */
static int
php_plphp_startup(sapi_module_struct *sapi_module)
{
	return php_module_startup(sapi_module, NULL, 0);
}

static void
php_plphp_log_messages(char *message)
{
	elog(LOG, "plphp: %s", message);
}

zend_function_entry spi_functions[] = {
	ZEND_FE(spi_exec_query, NULL)
	ZEND_FE(spi_fetch_row, NULL)
	{NULL, NULL, NULL}
};

zend_module_entry spi_module_entry = {
	STANDARD_MODULE_HEADER,
	"SPI module",
	spi_functions,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NO_VERSION_YET,
	STANDARD_MODULE_PROPERTIES
};

static sapi_module_struct plphp_sapi_module = {
	"plphp",					/* name */
	"PL/php PostgreSQL Handler",/* pretty name */

	php_plphp_startup,			/* startup */
	php_module_shutdown_wrapper,/* shutdown */

	NULL,						/* activate */
	NULL,						/* deactivate */

	sapi_plphp_ub_write,		/* unbuffered write */
	NULL,						/* flush */
	NULL,						/* get uid */
	NULL,						/* getenv */

	php_error,					/* error handler */

	NULL,						/* header handler */
	NULL,
	sapi_plphp_send_header,		/* send header handler */

	NULL,						/* read POST data */
	NULL,						/* read Cookies */

	NULL,						/* register server variables */
	php_plphp_log_messages,		/* Log message */

	NULL,						/* Block interrupts */
	NULL,						/* Unblock interrupts */
	STANDARD_SAPI_MODULE_PROPERTIES
};

/*
 * plphp_init_all()		- Initialize all
 */
static void
plphp_init_all(void)
{

	/************************************************************
	 * Execute postmaster-startup safe initialization
	 ************************************************************/
	if (plphp_first_call)
		plphp_init();

	/************************************************************
	 * Any other initialization that must be done each time a new
	 * backend starts -- currently none
	 ************************************************************/
}

void
plphp_init(void)
{
	/*
	 * Do initialization only once
	 */

	if (!plphp_first_call)
		return;

	plphp_sapi_module.phpinfo_as_text = 1;		/*output without html tags */
	sapi_startup(&plphp_sapi_module);
	if (php_module_startup(&plphp_sapi_module, NULL, 0) == FAILURE)
	{
		/*
		 * there is no way to see if we must call zend_ini_deactivate()
		 * since we cannot check if EG(ini_directives) has been initialised
		 * because the executor's constructor does not set initialize it.
		 * Apart from that there seems no need for zend_ini_deactivate() yet.
		 * So we goto out_err.
		 */
		elog(ERROR, "Could not startup plphp module");	/* use postgreSQL log for error */
	}
	/*
	 * init procedure cache
	 */
	MAKE_STD_ZVAL(plphp_proc_array);
	array_init(plphp_proc_array);
	plphp_first_call = false;


	zend_register_functions(
#if PHP_MAJOR_VERSION == 5
							NULL,
#endif
							spi_functions, NULL,
							MODULE_PERSISTENT TSRMLS_CC);

	PG(during_request_startup) = true;

	zend_first_try
	{
		/* Set some defaults */
		SG(options) |= SAPI_OPTION_NO_CHDIR;

		/* Hard coded defaults which cannot be overwritten in the ini file */
		INI_HARDCODED("register_argc_argv", "0");
		INI_HARDCODED("html_errors", "0");
		INI_HARDCODED("implicit_flush", "1");
		INI_HARDCODED("max_execution_time", "0");

		/* tell the engine we're in non-html mode */
		zend_uv.html_errors = false;

		/* not initialized but needed for several options */
		CG(in_compilation) = false;

		EG(uninitialized_zval_ptr) = NULL;

		if (php_request_startup(TSRMLS_C) == FAILURE)
		{
			SG(headers_sent) = 1;
			SG(request_info).no_headers = 1;
			/* Use Postgres log */
			elog(WARNING, "Could not startup plphp request.\n");
		}

		CG(interactive) = true;
		PG(during_request_startup) = true;
		plphp_first_call = false;

	}
	zend_catch
	{
		plphp_first_call = false;
		elog(ERROR, "plphp: fatal error...");
	}
	zend_end_try();
}

/*
 * plphp_call_handler
 *
 * The visible function of the PL interpreter.  The PostgreSQL function manager
 * and trigger manager call this function for execution of php procedures.
 */
Datum
plphp_call_handler(PG_FUNCTION_ARGS)
{
	Datum		retval;

	/*
	 * Initialize interpreter
	 */
	plphp_init_all();
	/*
	 * Connect to SPI manager
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to SPI manager")));

	if (CALLED_AS_TRIGGER(fcinfo))
	{
		/*
		 * Called as a trigger procedure
		 */
		retval = plphp_trigger_handler(fcinfo);
	}
	else
	{
		/*
		 * Called as a function
		 */
		retval = plphp_func_handler(fcinfo);
	}

	return retval;
}

Datum
plphp_validator(PG_FUNCTION_ARGS)
{
	/* Just a stub for now */

	/* The result of a validator is ignored */
	PG_RETURN_VOID();
}

static zval *
plphp_get_new(zval * array)
{
	zval	  **element;

	if (zend_hash_find
		(array->value.ht, "new", sizeof("new"),
		 (void **) & element) == SUCCESS)
	{
		if (Z_TYPE_P(element[0]) == IS_ARRAY)
		{
			return element[0];
		}
		else
			elog(ERROR, "plphp: field new must be an array");
	}
	else
		elog(ERROR, "plphp: field _TD[new] deleted, unable to modify tuple ");
	return NULL;
}

static int
plphp_attr_count(zval * array)
{
	/* WARNING -- this only works on simple arrays */
	return zend_hash_num_elements(Z_ARRVAL_P(array));
}

static int
plphp_get_rows_num(zval * array)
{
	zval	  **element;
	HashPosition pos;
	int			row = 0;
	int			rv = 0;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		rv++;
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) == IS_ARRAY)
				row++;
		}

	}
	else
		elog(ERROR, "plphp: wrong type: %i", array->type);
	return ((!row) ? rv : row);
}

static int
plphp_attr_count_r(zval *array)
{
	zval	  **element;
	HashPosition pos;
	int			ret = 0;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
				ret++;
			else
				ret += plphp_attr_count_r(element[0]);
		}
	}
	return ret;
}

static char **
plphp_get_attr_name(zval * array)
{
	char	  **rv;
	zval	  **element;
	HashPosition pos;
	int			cc = 0,
				i = 0;

	cc = plphp_attr_count(array);
	rv = (char **) palloc(cc * sizeof(char *));

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
			{
				rv[i] = (char *) palloc(pos->nKeyLength * sizeof(char));
				rv[i] = pos->arKey;
				i++;
			}
			else
				elog(ERROR, "plphp: input type must not be an array");
		}
	}
	else
		elog(ERROR, "plphp: input type must be an array");
	return rv;
}

static zval *
plphp_get_pelem(zval * array, char *key)
{
	zval	  **element;
	HashPosition pos;

	if (NULL == array)
	    return NULL;

	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
			{
				if (strcasecmp(pos->arKey, key) == 0)
				{
					return element[0];
				}
			}
		}
	}
	return NULL;
}

static char *
plphp_get_elem(zval * array, char *key)
{
	zval	   *element;
	char	   *ret;

	ret = palloc(64);

	element = plphp_get_pelem(array, key);

	if ((NULL != element) && (Z_TYPE_P(element) != IS_ARRAY))
	{
		if (Z_TYPE_P(element) == IS_LONG)
		{
			sprintf(ret, "%ld", element->value.lval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_DOUBLE)
		{
			sprintf(ret, "%e", element->value.dval);
			return ret;
		}
		else if (Z_TYPE_P(element) == IS_STRING)
		{
			return element->value.str.val;
		}
	}
	return NULL;
}

zval *
plphp_get_row(zval * array, int index)
{
	zval	  **element;
	HashPosition pos;
	int			row = 0;

	if (NULL != array)
		if (Z_TYPE_P(array) == IS_ARRAY)
		{

			for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
				 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
											   (void **) &element,
											   &pos) == SUCCESS;
				 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
			{

				if (Z_TYPE_P(element[0]) != IS_ARRAY)
				{
					if (row == index)
						return element[0];
					row++;
				}
				else
				{
					if (row == index)
						return element[0];
					row++;
				}
			}
		}
	return NULL;
}

static zval *
plphp_convert_from_cArray(char *input)
{
	zval	   *retval = NULL;
	char	   *work;
	int			i,
				arr_cnt = 0;
	char		buff[2];

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	for (i = 0; i < strlen(input); i++)
	{
		if (input[i] == '{')
			arr_cnt++;
	}

	work = (char *) palloc(strlen(input) + arr_cnt * 5 + 2);

	sprintf(work, "array(");

	for (i = 1; i < strlen(input); i++)
	{
		if (input[i] == '{')
			strcat(work, "array(");
		else if (input[i] == '}')
			strcat(work, ")");
		else
		{
			sprintf(buff, "%c", input[i]);
			strcat(work, buff);
		}
	}

	strcat(work, ";");

	if (zend_eval_string(work, retval, "plphp array input parametr" TSRMLS_CC)
		== FAILURE)
	{
		elog(ERROR, "plphp: convert to internal representation failure");
	}

	return retval;
}

static TupleDesc
get_function_tupdesc(Oid result_type, ReturnSetInfo *rsinfo)
{
	if (result_type == RECORDOID)
	{
		/*
		 * We must get the information from call context
		 */
		if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		return rsinfo->expectedDesc;
	}
	else
		/*
		 * ordinary composite type
		 */
		return lookup_rowtype_tupdesc(result_type, -1);
}

static int
plphp_convert_to_pg_array(zval * array, char *buffer)
{
	int			arr_size = 0;
	zval	  **element;
	HashPosition pos;
	char	   *cElem;
	int			i = 0;

	arr_size = plphp_attr_count(array);

	strcat(buffer, "{");
	if (Z_TYPE_P(array) == IS_ARRAY)
	{
		for (zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(array), &pos);
			 zend_hash_get_current_data_ex(Z_ARRVAL_P(array),
										   (void **) &element,
										   &pos) == SUCCESS;
			 zend_hash_move_forward_ex(Z_ARRVAL_P(array), &pos))
		{
			if (Z_TYPE_P(element[0]) != IS_ARRAY)
			{
				switch (Z_TYPE_P(element[0]))
				{
					case IS_LONG:
						cElem = palloc(255);
						sprintf(cElem, "%li", element[0]->value.lval);
						strcat(buffer, cElem);
						pfree(cElem);
						break;
					case IS_DOUBLE:
						cElem = palloc(255);
						sprintf(cElem, "%e", element[0]->value.dval);
						strcat(buffer, cElem);
						pfree(cElem);
						break;
					case IS_STRING:
						strcat(buffer, element[0]->value.str.val);
						break;
				}
			}
			else
			{
				plphp_convert_to_pg_array(element[0], buffer);
			}
			if (i != arr_size - 1)
				strcat(buffer, ",");
			i++;
		}
	}
	strcat(buffer, "}");
	return 1;
}

static HeapTuple
plphp_modify_tuple(zval * pltd, TriggerData *tdata)
{
	zval	   *plntup;
	char	  **plkeys;
	char	   *platt;
	char	   *plval;
	HeapTuple	rtup;
	int			natts,
				i;
	int		   *volatile modattrs;
	Datum	   *volatile modvalues;
	char	   *volatile modnulls;
	TupleDesc	tupdesc;
	HeapTuple	typetup;

	tupdesc = tdata->tg_relation->rd_att;

	modattrs = NULL;
	modvalues = NULL;
	modnulls = NULL;
	tupdesc = tdata->tg_relation->rd_att;

	plntup = plphp_get_new(pltd);

	if (Z_TYPE_P(plntup) != IS_ARRAY)
		elog(ERROR, "plphp: _TD[\"new\"] is not an array");

	plkeys = plphp_get_attr_name(plntup);
	natts = plphp_attr_count(plntup);

	if (natts != tupdesc->natts)
		elog(ERROR, "plphp: _TD[\"new\"] has an incorrect number of keys.");

	modattrs = palloc0(natts * sizeof(int));
	modvalues = palloc0(natts * sizeof(Datum));
	modnulls = palloc0(natts * sizeof(char));

	for (i = 0; i < natts; i++)
	{
		FmgrInfo	finfo;
		Oid			typinput;
		Oid			typelem;
		int			atti,
					attn;

		platt = plkeys[i];
		attn = modattrs[i] = SPI_fnumber(tupdesc, platt);

		if (attn == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "plperl: invalid attribute `%s' in tuple.", platt);
		atti = attn - 1;

		plval = plphp_get_elem(plntup, platt);

		if (plval == NULL)
			elog(FATAL, "plphp: interpreter is probably corrupted");

		typetup =
			SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(tupdesc->attrs[atti]->atttypid),
						   0, 0, 0);
		typinput = ((Form_pg_type) GETSTRUCT(typetup))->typinput;
		typelem = ((Form_pg_type) GETSTRUCT(typetup))->typelem;
		ReleaseSysCache(typetup);
		fmgr_info(typinput, &finfo);

		if (plval)
		{
			modvalues[i] = FunctionCall3(&finfo,
										 CStringGetDatum(plval),
										 ObjectIdGetDatum(typelem),
										 Int32GetDatum(tupdesc->attrs[atti]->
													   atttypmod));
			modnulls[i] = ' ';
		}
		else
		{
			modvalues[i] = (Datum) 0;
			modnulls[i] = 'n';
		}
	}

	rtup =
		SPI_modifytuple(tdata->tg_relation, tdata->tg_trigtuple, natts,
						modattrs, modvalues, modnulls);

	pfree(modattrs);
	pfree(modvalues);
	pfree(modnulls);

	if (rtup == NULL)
		elog(ERROR, "plphp: SPI_modifytuple failed -- error:  %d", SPI_result);

	return rtup;
}

static zval *
plphp_trig_build_args(FunctionCallInfo fcinfo)
{
	zval	   *retval;
	int			i;

	TriggerData *tdata;
	TupleDesc	tupdesc;

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	add_assoc_string(retval, "name", tdata->tg_trigger->tgname, 1);
	add_assoc_string(retval, "relid",
					 DatumGetCString(DirectFunctionCall1
									 (oidout,
									  ObjectIdGetDatum(tdata->tg_relation->
													   rd_id))), 1);

	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
	{
		zval	   *hashref;

		add_assoc_string(retval, "event", "INSERT", 1);
		hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
		zend_hash_update(retval->value.ht, "new", strlen("new") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);
	}
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
	{
		zval	   *hashref;

		add_assoc_string(retval, "event", "DELETE", 1);
		hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
		zend_hash_update(retval->value.ht, "old", strlen("old") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);
	}
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
	{
		zval	   *hashref;

		add_assoc_string(retval, "event", "UPDATE", 1);

		hashref = plphp_build_tuple_argument(tdata->tg_newtuple, tupdesc);
		zend_hash_update(retval->value.ht, "new", strlen("new") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);

		hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
		zend_hash_update(retval->value.ht, "old", strlen("old") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);
	}
	else
		add_assoc_string(retval, "event", "UNKNOWN", 1);

	add_assoc_long(retval, "argc", tdata->tg_trigger->tgnargs);

	if (tdata->tg_trigger->tgnargs > 0)
	{
		zval	   *hashref;

		MAKE_STD_ZVAL(hashref);
		array_init(hashref);

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
		{
			add_index_string(hashref, i, tdata->tg_trigger->tgargs[i], 1);
		}
		zend_hash_update(retval->value.ht, "args", strlen("args") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);
	}

	add_assoc_string(retval, "relname", SPI_getrelname(tdata->tg_relation), 1);

	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		add_assoc_string(retval, "when", "BEFORE", 1);
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		add_assoc_string(retval, "when", "AFTER", 1);
	else
		add_assoc_string(retval, "when", "UNKNOWN", 1);

	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		add_assoc_string(retval, "level", "ROW", 1);
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		add_assoc_string(retval, "level", "STATEMENT", 1);
	else
		add_assoc_string(retval, "level", "UNKNOWN", 1);

	return retval;
}

/*
 * plphp_trigger_handler()		- Handler for trigger function calls
 */

static Datum
plphp_trigger_handler(PG_FUNCTION_ARGS)
{
	zval	   *phpret = NULL;
	zval	   *zTrigData = NULL;
	plphp_proc_desc *desc;
	Datum		retval;

	/*
	 * Find or compile the function
	 */
	desc = compile_plphp_function(fcinfo->flinfo->fn_oid, true);
	PG(safe_mode) = desc->lanpltrusted;

	zTrigData = plphp_trig_build_args(fcinfo);
	phpret = plphp_call_php_trig(desc, fcinfo, zTrigData);

	/*
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	retval = (Datum) 0;

	if (phpret)
	{
		HeapTuple	trv = NULL;
		TriggerData *trigdata = (TriggerData *) fcinfo->context;
		char	   *srv;

		switch (phpret->type)
		{
			case IS_STRING:
				srv = phpret->value.str.val;
				if (strcasecmp(srv, "SKIP") == 0)
					trv = NULL;
				else if (strcasecmp(srv, "MODIFY") == 0)
				{

					if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
					{
						trv = plphp_modify_tuple(zTrigData, trigdata);
						retval = PointerGetDatum(trv);
					}
					else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
					{
						trv = plphp_modify_tuple(zTrigData, trigdata);
						retval = PointerGetDatum(trv);
					}
					else
					{
						trv = NULL;
						elog(WARNING,
							 "plphp: Ignoring modified tuple in DELETE trigger");
					}
				}
				else
				{
					trv = NULL;
					elog(ERROR,
						 "plphp: Expected return to be 'SKIP' or 'MODIFY'");
				}
				break;
			case IS_NULL:
				if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
					retval = (Datum) trigdata->tg_trigtuple;
				else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
					retval = (Datum) trigdata->tg_newtuple;
				else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
					retval = (Datum) trigdata->tg_trigtuple;

				break;
			default:
				elog(ERROR,
					 "plphp: Expected trigger to return None or a String");
		}
	}
	else
		elog(ERROR, "plphp: error during execute plphp trigger function");

	return retval;
}

/*
 * plphp_func_handler()		- Handler for regular function calls
 */

static Datum
plphp_func_handler(PG_FUNCTION_ARGS)
{
	zval	   *phpret = NULL;
	plphp_proc_desc *desc;
	Datum		retval;
	char	   *retvalbuffer = NULL;

	/*
	 * Find or compile the function
	 */
	desc = compile_plphp_function(fcinfo->flinfo->fn_oid, false);

	PG(safe_mode) = desc->lanpltrusted;

	/*
	 * Call the PHP function if not returning set
	 */
	if (!(desc->ret_type & PL_SET))
	{
		phpret = plphp_call_php_func(desc, fcinfo);
	}
	else
	{
		if (SRF_IS_FIRSTCALL())
			srf_phpret = plphp_call_php_func(desc, fcinfo);
		phpret = srf_phpret;
	}
	/*
	 * Disconnect from SPI manager and then create the return
	 * values datum (if the input function does a palloc for it
	 * this must not be allocated in the SPI memory context
	 * because SPI_finish would free it).
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could disconnect from SPI manager")));
	retval = (Datum) 0;

	if (desc->ret_type & PL_PSEUDO)
	{
		HeapTuple	retTypeTup;
		Form_pg_type retTypeStruct;

		retTypeTup = SearchSysCache(TYPEOID,
									ObjectIdGetDatum(get_fn_expr_rettype(fcinfo->flinfo)),
									0, 0, 0);
		retTypeStruct = (Form_pg_type) GETSTRUCT(retTypeTup);
		perm_fmgr_info(retTypeStruct->typinput, &(desc->result_in_func));
		desc->result_typioparam = retTypeStruct->typelem;
		ReleaseSysCache(retTypeTup);
	}

	if (phpret)
	{
		switch (phpret->type)
		{
			case IS_LONG:
				retvalbuffer = (char *) palloc(256);
				sprintf(retvalbuffer, "%ld", phpret->value.lval);
				break;
			case IS_DOUBLE:
				retvalbuffer = (char *) palloc(256);
				sprintf(retvalbuffer, "%e", phpret->value.dval);
				break;
			case IS_STRING:
				retvalbuffer = (char *) palloc(phpret->value.str.len + 1);
				snprintf(retvalbuffer, phpret->value.str.len + 1, "%s",
						 phpret->value.str.val);
				break;
			case IS_ARRAY:
				if ((desc->ret_type & PL_ARRAY) && !(desc->ret_type & PL_SET))
				{
					/* FIXME -- buffer overrun here */
					int		length = plphp_attr_count_r(phpret);
					retvalbuffer = (char *) palloc(length * 10);
					memset(retvalbuffer, 0x0, length * 10);
					plphp_convert_to_pg_array(phpret, retvalbuffer);
				}
				else if ((desc->ret_type & PL_TUPLE) &&
						 !(desc->ret_type & PL_SET))
				{
					TupleDesc	td;
					int			i;
					char	  **values;
					char	   *key,
							   *val;
					AttInMetadata *attinmeta;
					HeapTuple	tup;

					if (desc->ret_type & PL_PSEUDO)
					{
						ReturnSetInfo *rsinfo;

						rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
						td = get_function_tupdesc(desc->ret_oid,
												  (ReturnSetInfo *) fcinfo->resultinfo);
					}
					else
					{
						td = lookup_rowtype_tupdesc(desc->ret_oid, (int32) -1);
					}

					if (!td)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no TupleDesc info available")));

					values = (char **) palloc(td->natts * sizeof(char *));

					for (i = 0; i < td->natts; i++)
					{

						key = SPI_fname(td, i + 1);
						val = plphp_get_elem(phpret, key);
						if (val)
							values[i] = val;
						else
							values[i] = NULL;
					}
					attinmeta = TupleDescGetAttInMetadata(td);
					tup = BuildTupleFromCStrings(attinmeta, values);
					retval = HeapTupleGetDatum(tup);

				}
				else if (desc->ret_type & PL_SET)
				{
					retval = plphp_srf_handler(fcinfo, desc);
				}
				else
					elog(ERROR,
						 "this plphp function cannot return arrays to PostgreSQL.");
				break;
			case IS_NULL:
				fcinfo->isnull = true;
				break;
			default:
				elog(WARNING,
					 "plphp functions cannot return this type %i to PostgreSQL.",
					 phpret->type);
				fcinfo->isnull = true;

		}
	}
	else
	{
		fcinfo->isnull = true;
		retval = (Datum) 0;
	}

	if ((!fcinfo->isnull) && !(desc->ret_type & PL_TUPLE) &&
		!(desc->ret_type & PL_SET))
	{
		retval = FunctionCall3(&desc->result_in_func,
							   PointerGetDatum(retvalbuffer),
							   ObjectIdGetDatum(desc->result_typioparam),
							   Int32GetDatum(-1));
		pfree(retvalbuffer);
	}
	return retval;
}

zval *
plphp_hash_from_tuple(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	char	   *attname = NULL;
	char	   *attdata = NULL;

	zval	   *array;

	MAKE_STD_ZVAL(array);
	array_init(array);

	for (i = 0; i < tupdesc->natts; i++)
	{

		/************************************************************
		* Get the attribute name
		************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		* Get the attributes value
		************************************************************/
		attdata = SPI_getvalue(tuple, tupdesc, i + 1);
		if (attdata)
			add_assoc_string(array, attname, attdata, 1);
		else
			add_assoc_null(array, attname);
	}
	return array;
}

ZEND_FUNCTION(spi_exec_query)
{
	char	   *query;
	int			query_len;
	long		status;
	long		limit;
	zval	   *rv = NULL;
	char	   *pointer = NULL;

	if ((ZEND_NUM_ARGS() > 2) || (ZEND_NUM_ARGS() < 1))
		WRONG_PARAM_COUNT;

	if (ZEND_NUM_ARGS() == 2)
	{

		if (zend_parse_parameters
			(ZEND_NUM_ARGS()TSRMLS_CC, "s|l", &query, &query_len,
			 &limit) == FAILURE)
		{

			zend_error(E_WARNING, "Can not parse parameters in %s",
					   get_active_function_name(TSRMLS_C));
			return;
		}
	}
	else
	{

		if (zend_parse_parameters
			(ZEND_NUM_ARGS()TSRMLS_CC, "s", &query, &query_len) == FAILURE)
		{

			zend_error(E_WARNING, "Can not parse parameters in %s",
					   get_active_function_name(TSRMLS_C));
			return;
		}
		limit = 0;
	}

	status = SPI_exec(query, limit);

	MAKE_STD_ZVAL(rv);
	array_init(rv);
	add_assoc_long(rv, "processed", SPI_processed);
	add_assoc_string(rv, "status", (char *) SPI_result_code_string(status), 0);

	if (status == SPI_OK_SELECT)
	{
		add_assoc_long(rv, "fetch_counter", 0);
		/*
		 * Save pointer to the SPI_tuptable
		 */
		pointer = (char *) palloc(sizeof(SPITupleTable *));
		sprintf(pointer, "%p", (void *) SPI_tuptable);
		add_assoc_string(rv, "res", (char *) pointer, 1);
	}

	*return_value = *rv;
	zval_copy_ctor(return_value);

	return;
}

ZEND_FUNCTION(spi_fetch_row)
{
	zval	   *param;
	zval	   *tmp;
	zval	   *row = NULL;
	char	   *pointer = NULL;
	SPITupleTable *tup_table;
	long int	processed,
				counter;

	if (ZEND_NUM_ARGS() != 1)
		WRONG_PARAM_COUNT;
	if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "a", &param) ==
		FAILURE)
	{
		zend_error(E_WARNING, "Can not parse parameters in %s",
				   get_active_function_name(TSRMLS_C));
		return;
	}
	pointer = plphp_get_elem(param, "res");
	sscanf(pointer, "%p", &tup_table);

	tmp = plphp_get_pelem(param, "processed");
	processed = tmp->value.lval;

	tmp = plphp_get_pelem(param, "fetch_counter");
	counter = tmp->value.lval;

	if (counter < processed)
	{
		row =
			plphp_hash_from_tuple(tup_table->vals[counter],
								  tup_table->tupdesc);
		tmp->value.lval++;

		*return_value = *row;
		zval_copy_ctor(return_value);
	}
	else
		RETURN_FALSE;

	return;
}

/*
 * compile_plphp_function	- compile (or hopefully just look up) function
 */
static plphp_proc_desc *
compile_plphp_function(Oid fn_oid, int is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	int			proname_len;
	plphp_proc_desc *prodesc = NULL;
	int			i;

	/*
	 * We'll need the pg_proc tuple in any case... 
	 */
	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Build our internal proc name from the functions Oid
	 */

	if (!is_trigger)
		snprintf(internal_proname, sizeof(internal_proname), "plphp_proc_%u",
				 fn_oid);
	else
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u_trigger", fn_oid);

	proname_len = strlen(internal_proname);

	/*
	 * Lookup the internal proc name in the hashtable
	 */
	{
		char	   *pointer = NULL;
		bool		uptodate;

		pointer = plphp_get_elem(plphp_proc_array, internal_proname);
		if (pointer)
		{
			sscanf(pointer, "%p", &prodesc);

			uptodate =
				(prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
				 prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

			if (!uptodate)
			{
				/*
				 * need we delete old entry?
				 */
				prodesc = NULL;
			}

		}
	}

	if (prodesc == NULL)
	{
		HeapTuple	langTup;
		HeapTuple	typeTup;
		Form_pg_language langStruct;
		Form_pg_type typeStruct;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;
		char	   *complete_proc_source;

		/*
		 * Allocate a new procedure description block
		 */
		prodesc = (plphp_proc_desc *) malloc(sizeof(plphp_proc_desc));
		if (prodesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		MemSet(prodesc, 0, sizeof(plphp_proc_desc));
		prodesc->proname = strdup(internal_proname);
		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);

		/*
		 * Lookup the pg_language tuple by Oid
		 */

		langTup = SearchSysCache(LANGOID,
								 ObjectIdGetDatum(procStruct->prolang),
								 0, 0, 0);
		if (!HeapTupleIsValid(langTup))
		{
			free(prodesc->proname);
			free(prodesc);
			elog(ERROR, "cache lookup failed for language %u",
				 procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

		/*
		 * Get the required information for input conversion of the
		 * return value.
		 */
		if (!is_trigger)
		{
			typeTup = SearchSysCache(TYPEOID,
									 ObjectIdGetDatum(procStruct->prorettype),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
			{
				free(prodesc->proname);
				free(prodesc);
				elog(ERROR, "cache lookup failed for type %u",
					 procStruct->prorettype);
			}
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

			/*
			 * Disallow pseudotype result, except:
			 * VOID, RECORD, ANYELEMENT or ANYARRAY
			 */
			if (typeStruct->typtype == 'p')
			{
				if ((procStruct->prorettype == VOIDOID) ||
					(procStruct->prorettype == RECORDOID) ||
					(procStruct->prorettype == ANYELEMENTOID) ||
					(procStruct->prorettype == ANYARRAYOID))
				{
					/*
					 * okay
					 */
					prodesc->ret_type = prodesc->ret_type | PL_PSEUDO;
				}
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg
							 ("trigger functions may only be called as triggers")));
				}
				else
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plperl functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}
			if (procStruct->proretset)
				prodesc->ret_type = prodesc->ret_type | PL_SET;

			prodesc->ret_oid = procStruct->prorettype;

			if (typeStruct->typtype == 'c' ||
				procStruct->prorettype == RECORDOID)
			{
				prodesc->ret_type = prodesc->ret_type | PL_TUPLE;
			}

			if (procStruct->prorettype != ANYARRAYOID)
			{
				/* true, if function returns "true" array */
				if (typeStruct->typlen == -1 && typeStruct->typelem)
					prodesc->ret_type = prodesc->ret_type | PL_ARRAY;
			}
			else
				prodesc->ret_type = prodesc->ret_type | PL_ARRAY;

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);
		}

		/*
		 * Get the required information for output conversion
		 * of all procedure arguments
		 */
		if (!is_trigger)
		{
			prodesc->nargs = procStruct->pronargs;

			for (i = 0; i < prodesc->nargs; i++)
			{
				typeTup = SearchSysCache(TYPEOID,
										 ObjectIdGetDatum(procStruct->proargtypes.values[i]),
										 0, 0, 0);
				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->proname);
					free(prodesc);
					elog(ERROR, "cache lookup failed for type %u",
						 procStruct->proargtypes.values[i]);
				}
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/*
				 * Disallow pseudotype argument
				 */
				if (typeStruct->typtype == 'p')
				{
					prodesc->arg_is_p[i] = true;
				}
				else
					prodesc->arg_is_p[i] = false;

				if (typeStruct->typtype == 'c')
					prodesc->arg_is_rowtype[i] = true;
				else
				{
					prodesc->arg_is_rowtype[i] = false;
					perm_fmgr_info(typeStruct->typoutput,
								   &(prodesc->arg_out_func[i]));
					prodesc->arg_typioparam[i] = getTypeIOParam(typeTup);
				}

				ReleaseSysCache(typeTup);
			}
		}

		/*
		 * create the text of the anonymous subroutine.
		 * we do not use a named subroutine so that we can call directly
		 * through the reference.
		 *
		 */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		proc_source = DatumGetCString(DirectFunctionCall1(textout,
														  prosrcdatum));

		/*
		 * Create the procedure in the interpreter
		 */

		complete_proc_source =
			(char *) palloc(strlen(proc_source) + proname_len +
							strlen("function \0($args, $argc){\0}") + 20);

		if (!is_trigger)
			sprintf(complete_proc_source, "function %s($args, $argc){%s}",
					internal_proname, proc_source);
		else
			sprintf(complete_proc_source, "function %s($_TD){%s}",
					internal_proname, proc_source);

		zend_hash_del(CG(function_table), prodesc->proname,
					  strlen(prodesc->proname) + 1);
		{
			char	   *pointer = NULL;

			pointer = (char *) palloc(100);
			sprintf(pointer, "%p", (void *) prodesc);
			add_assoc_string(plphp_proc_array, internal_proname,
							 (char *) pointer, 1);
		}

		if (zend_eval_string
			(complete_proc_source, NULL,
			 "plphp function source" TSRMLS_CC) == FAILURE)
		{
			elog(ERROR, "plphp: unable to register function \"%s\"",
				 prodesc->proname);
		}

		pfree(complete_proc_source);
	}
	ReleaseSysCache(procTup);

	return prodesc;
}

static zval *
plphp_func_build_args(plphp_proc_desc * desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	int			i;

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	if (desc->nargs)
	{

		for (i = 0; i < desc->nargs; i++)
		{
			if (desc->arg_is_p[i])
			{
				HeapTuple	typeTup;
				Form_pg_type typeStruct;

				typeTup = SearchSysCache(TYPEOID,
										 ObjectIdGetDatum(get_fn_expr_argtype
														  (fcinfo->flinfo, i)),
										 0, 0, 0);
				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
				perm_fmgr_info(typeStruct->typoutput,
							   &(desc->arg_out_func[i]));
				desc->arg_typioparam[i] = typeStruct->typelem;
				ReleaseSysCache(typeTup);
			}

			if (desc->arg_is_rowtype[i])
			{
				if (fcinfo->argnull[i])
					add_next_index_unset(retval);
				else
				{
					HeapTupleHeader td;
					Oid			tupType;
					int32		tupTypmod;
					TupleDesc	tupdesc;
					HeapTupleData tmptup;
					zval	   *hashref;

					td = DatumGetHeapTupleHeader(fcinfo->arg[i]);
					/*
					 * Extract rowtype info and find a tupdesc
					 */
					tupType = HeapTupleHeaderGetTypeId(td);
					tupTypmod = HeapTupleHeaderGetTypMod(td);
					tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
					/*
					 * Build a temporary HeapTuple control structure
					 */
					tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
					tmptup.t_data = td;

					/*
					 * plphp_build_tuple_argument
					 */
					hashref = plphp_build_tuple_argument(&tmptup, tupdesc);
					zend_hash_next_index_insert(retval->value.ht,
												(void *) &hashref,
												sizeof(zval *), NULL);
				}
			}
			else
			{
				if (fcinfo->argnull[i])
					add_next_index_unset(retval);
				else
				{
					char	   *tmp;

					tmp =
						DatumGetCString(FunctionCall3
										(&(desc->arg_out_func[i]),
										 fcinfo->arg[i],
										 ObjectIdGetDatum(desc->
														  arg_typioparam[i]),
										 Int32GetDatum(-1)));
					if (tmp[0] == '{')
					{
						/* some hack to determine that the array type used */
						zval	   *hashref;

						hashref = plphp_convert_from_cArray(tmp);
						zend_hash_next_index_insert(retval->value.ht,
													(void *) &hashref,
													sizeof(zval *), NULL);
					}
					else
						add_next_index_string(retval, tmp, 1);
					/*
					 * FIXME strnge but some time fith simlpe params this crush backend :(
					 * pfree(tmp);
					 */
				}
			}
		}
	}

	return retval;
}

static zval *
plphp_call_php_func(plphp_proc_desc * desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	zval	   *Zargs;
	zval	   *Zargc;
	char		_call[64];

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	MAKE_STD_ZVAL(Zargs);
	array_init(Zargs);
	MAKE_STD_ZVAL(Zargc);
	ZVAL_LONG(Zargc, desc->nargs);

	Zargs = plphp_func_build_args(desc, fcinfo);

	ZEND_SET_SYMBOL(&EG(symbol_table), "_PLA", Zargs);
	ZEND_SET_SYMBOL(&EG(symbol_table), "_PLAC", Zargc);
	sprintf(_call, "plphp_proc_%u($_PLA, $_PLAC);", fcinfo->flinfo->fn_oid);

	zend_first_try
	{
		if (zend_eval_string(_call, retval, "plphp function call" TSRMLS_CC) ==
			FAILURE)
		{
			elog(ERROR, "plphp: function call - failure");
		}

	}
	zend_catch
	{
		elog(ERROR, "plphp: fatal error...");
	}
	zend_end_try();

	return retval;
}

static zval *
plphp_call_php_trig(plphp_proc_desc * desc, FunctionCallInfo fcinfo,
					zval * hvTD)
{
	zval	   *retval;
	char		_call[64];

	MAKE_STD_ZVAL(retval);
	retval->type = IS_NULL;

	ZEND_SET_SYMBOL(&EG(symbol_table), "_trigA", hvTD);
	sprintf(_call, "plphp_proc_%u_trigger(&$_trigA);", fcinfo->flinfo->fn_oid);

	zend_first_try
	{
		if (zend_eval_string(_call, retval, "plphp trigger call" TSRMLS_CC) ==
			FAILURE)
		{
			elog(ERROR, "plphp: trigger call - failure");
		}

	}
	zend_catch
	{
		elog(ERROR, "plphp: fatal error...");
	}
	zend_end_try();

	return retval;
}

/*
 * plphp_build_tuple_argument() - Build an array
 *				  from all attributes of a given tuple
 */
static zval *
plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc)
{
	int			i;
	zval	   *output;
	Datum		attr;
	bool		isnull;
	char	   *attname;
	char	   *outputstr;
	HeapTuple	typeTup;
	Oid			typoutput;
	Oid			typioparam;

	MAKE_STD_ZVAL(output);
	array_init(output);

	for (i = 0; i < tupdesc->natts; i++)
	{
		/*
		 * ignore dropped attributes
		 */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/************************************************************
		 * Get the attribute name
		 ************************************************************/
		attname = tupdesc->attrs[i]->attname.data;

		/************************************************************
		 * Get the attributes value
		 ************************************************************/
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/************************************************************
		 *	If it is null it will be set to undef in the hash.
		 ************************************************************/
		if (isnull)
		{
			add_next_index_unset(output);
			continue;
		}

		/************************************************************
		 * Lookup the attribute type in the syscache
		 * for the output function
		 ************************************************************/
		typeTup = SearchSysCache(TYPEOID,
								 ObjectIdGetDatum(tupdesc->attrs[i]->atttypid),
								 0, 0, 0);
		if (!HeapTupleIsValid(typeTup))
		{
			elog(ERROR, "cache lookup failed for type %u",
				 tupdesc->attrs[i]->atttypid);
		}

		typoutput = ((Form_pg_type) GETSTRUCT(typeTup))->typoutput;
		typioparam = getTypeIOParam(typeTup);
		ReleaseSysCache(typeTup);

		/************************************************************
		 * Append the attribute name and the value to the list.
		 ************************************************************/
		outputstr = DatumGetCString(OidFunctionCall3(typoutput,
													 attr,
													 ObjectIdGetDatum
													 (typioparam),
													 Int32GetDatum(tupdesc->
																   attrs[i]->
																   atttypmod)));
		add_assoc_string(output, attname, outputstr, 1);
		pfree(outputstr);
	}

	return output;
}

static Datum
plphp_srf_handler(PG_FUNCTION_ARGS, plphp_proc_desc * prodesc)
{

	FuncCallContext *funcctx;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	AttInMetadata *attinmeta = NULL;
	int			call_cntr;
	int			max_calls;
	char	  **values = NULL;

	if (SRF_IS_FIRSTCALL())
		if (srf_phpret->type != IS_ARRAY)
			elog(ERROR, "plphp: this function must return reference to array");

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (prodesc->ret_type & PL_TUPLE)
		{
			/*
			 * Build a tuple description
			 */
			tupdesc =
				get_function_tupdesc(prodesc->ret_oid,
									 (ReturnSetInfo *) fcinfo->resultinfo);
			tupdesc = CreateTupleDescCopy(tupdesc);

			/*
			 * total number of tuples to be returned
			 */
			funcctx->max_calls = plphp_get_rows_num(srf_phpret);

			funcctx->tuple_desc = tupdesc;

			/*
			 * allocate a slot for a tuple with this tupdesc
			 */
			slot = TupleDescGetSlot(tupdesc);
			/*
			 * assign slot to function context
			 */
			funcctx->slot = slot;
			/*
			 * generate attribute metadata needed later to produce tuples from raw
			 * C strings
			 */
			attinmeta = TupleDescGetAttInMetadata(tupdesc);
			funcctx->attinmeta = attinmeta;
		}
		else
			funcctx->max_calls = plphp_attr_count(srf_phpret);

		MemoryContextSwitchTo(oldcontext);
	}
	funcctx = SRF_PERCALL_SETUP();

	if (prodesc->ret_type & PL_TUPLE)
	{
		slot = funcctx->slot;
		attinmeta = funcctx->attinmeta;
	}

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	tupdesc = funcctx->tuple_desc;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		Datum		result;
		int			i;
		zval	   *zRow;

		if (prodesc->ret_type & PL_TUPLE)
		{
			HeapTuple	tuple;
			char	   *key,
					   *val;

			/*
			 * Prepare a values array for storage in our slot.
			 * This should be an array of C strings which will
			 * be processed later by the type input functions.
			 */
			values = (char **) palloc(tupdesc->natts * sizeof(char *));
			zRow = plphp_get_row(srf_phpret, call_cntr);

			for (i = 0; i < tupdesc->natts; i++)
			{
				key = SPI_fname(tupdesc, i + 1);
				val = plphp_get_elem(zRow, key);
				if (val)
					values[i] = val;
				else
					values[i] = NULL;
			}

			/*
			 * build a tuple
			 */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			/*
			 * make the tuple into a datum
			 */
			result = TupleGetDatum(slot, tuple);
		}
		else
		{
			char	   *retvalbuffer = NULL;

			zRow = plphp_get_row(srf_phpret, call_cntr);
			switch (zRow->type)
			{
				case IS_LONG:
					retvalbuffer = (char *) palloc(255);
					memset(retvalbuffer, 0x0, 255);
					sprintf(retvalbuffer, "%ld", zRow->value.lval);
					break;
				case IS_DOUBLE:
					retvalbuffer = (char *) palloc(255);
					memset(retvalbuffer, 0x0, 255);
					sprintf(retvalbuffer, "%e", zRow->value.dval);
					break;
				case IS_STRING:
					retvalbuffer = (char *) palloc(zRow->value.str.len + 1);
					memset(retvalbuffer, 0x0, strlen(retvalbuffer));
					sprintf(retvalbuffer, "%s", zRow->value.str.val);
					break;
				case IS_ARRAY:
					retvalbuffer =
						(char *) palloc(plphp_attr_count_r(zRow) * 2);
					memset(retvalbuffer, 0x0, strlen(retvalbuffer));
					plphp_convert_to_pg_array(zRow, retvalbuffer);
					break;
				case IS_NULL:
					fcinfo->isnull = true;
					break;
				default:
					fcinfo->isnull = true;
					elog(WARNING,
						 "plphp functions cannot return this type %i to PostgreSQL.",
						 zRow->type);
			}

			if (!fcinfo->isnull)
			{
				fcinfo->isnull = false;
				result = FunctionCall3(&prodesc->result_in_func,
									   PointerGetDatum(retvalbuffer),
									   ObjectIdGetDatum(prodesc->
														result_typioparam),
									   Int32GetDatum(-1));
				pfree(retvalbuffer);
			}
			else
			{
				fcinfo->isnull = true;
				result = (Datum) 0;
			}
		}
		SRF_RETURN_NEXT(funcctx, result);
		fcinfo->isnull = false;
	}
	else
	{							/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
