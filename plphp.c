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
 *********************************************************************
 */

/* Our own stuff first */
#include "plphp_io.h"

/* PostgreSQL stuff */

#include "catalog/catversion.h"
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

/* Check for PostgreSQL version */
#if (CATALOG_VERSION_NO == 200411041)
#define PG_VERSION_80_COMPAT
#elif (CATALOG_VERSION_NO >= 200510211)
#define PG_VERSION_81_COMPAT
#else
#error "Unrecognized PostgreSQL version"
#endif


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

static Datum plphp_func_handler(FunctionCallInfo fcinfo);
static Datum plphp_trigger_handler(FunctionCallInfo fcinfo);

static plphp_proc_desc *plphp_compile_function(Oid, int);
static zval *plphp_build_tuple_argument(HeapTuple tuple, TupleDesc tupdesc);
static zval *plphp_call_php_func(plphp_proc_desc *, FunctionCallInfo);
static zval *plphp_call_php_trig(plphp_proc_desc *, FunctionCallInfo, zval *);

static Datum plphp_srf_handler(PG_FUNCTION_ARGS, plphp_proc_desc *);

static void plphp_elog(int elevel, char *fmt, ...);
static void plphp_error_cb(int type, const char *filename, const uint lineno,
								  const char *fmt, va_list args);

ZEND_FUNCTION(spi_exec_query);
ZEND_FUNCTION(spi_fetch_row);
ZEND_FUNCTION(pg_raise);

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
		return 0;

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
			php_handle_aborted_connection();

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

/* FIXME -- this is a misnomer now. */
zend_function_entry spi_functions[] = {
	ZEND_FE(spi_exec_query, NULL)
	ZEND_FE(spi_fetch_row, NULL)
	ZEND_FE(pg_raise, NULL)
	{NULL, NULL, NULL}
};

/* FIXME -- this thing is unused ... */
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
 *
 * XXX This is called each time a function is invoked.
 */
static void
plphp_init_all(void)
{
	/* Execute postmaster-startup safe initialization */
	if (plphp_first_call)
		plphp_init();

	/*
	 * Any other initialization that must be done each time a new
	 * backend starts -- currently none.
	 */
}

/*
 * This function must not be static, so that it can be used in
 * preload_libraries.  If it is, it will be called by postmaster;
 * otherwise it will be called by each backend the first time a
 * function is called.
 */
void
plphp_init(void)
{
	/* Do initialization only once */
	if (!plphp_first_call)
		return;

	/* Omit HTML tags from output */
	plphp_sapi_module.phpinfo_as_text = 1;
	sapi_startup(&plphp_sapi_module);

	if (php_module_startup(&plphp_sapi_module, NULL, 0) == FAILURE)
		elog(ERROR, "Could not startup plphp module");	

	/*
	 * FIXME -- Figure out what this comment is supposed to mean:
	 *
	 * There is no way to see if we must call zend_ini_deactivate()
	 * since we cannot check if EG(ini_directives) has been initialised
	 * because the executor's constructor does not initialize it.
	 * Apart from that there seems no need for zend_ini_deactivate() yet.
	 * So we error out.
	 */

	/* Init procedure cache */
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

	zend_try
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
			elog(WARNING, "plphp: could not start up plphp request");
		}

		CG(interactive) = true;
		PG(during_request_startup) = true;
		plphp_first_call = false;

		zend_error_cb = plphp_error_cb;
	}
	zend_catch
	{
		plphp_first_call = false;
		elog(ERROR, "fatal error during PL/php initialization");
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

	/* Initialize interpreter */
	plphp_init_all();

	/* Connect to SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to SPI manager")));

	if (CALLED_AS_TRIGGER(fcinfo))
		/* called as a trigger procedure */
		retval = plphp_trigger_handler(fcinfo);
	else
		/* Called as a function */
		retval = plphp_func_handler(fcinfo);

	return retval;
}

/*
 * plphp_validator
 *
 * 		Validator function for checking the function's syntax at creation
 * 		time
 */
Datum
plphp_validator(PG_FUNCTION_ARGS)
{
	Oid				funcoid = PG_GETARG_OID(0);
	Form_pg_proc	procForm;
	HeapTuple		procTup;
	char			tmpname[32];
	char			funcname[NAMEDATALEN];
	char		   *tmpsrc,
				   *prosrc;
	Datum			prosrcdatum;
	bool			isnull;

	/* Initialize interpreter */
	plphp_init_all();

	/* Grab the pg_proc tuple */
	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(funcoid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", funcoid);

	procForm = (Form_pg_proc) GETSTRUCT(procTup);

	/* Get the function source code */
	prosrcdatum = SysCacheGetAttr(PROCOID,
								  procTup,
								  Anum_pg_proc_prosrc,
								  &isnull);
	if (isnull)
		plphp_elog(ERROR, "cache lookup yielded NULL prosrc");
	prosrc = DatumGetCString(DirectFunctionCall1(textout,
												 prosrcdatum));

	/* Get the function name, for the error message */
	StrNCpy(funcname, NameStr(procForm->proname), NAMEDATALEN);
	ReleaseSysCache(procTup);

	/* Create a PHP function creation statement */
	snprintf(tmpname, sizeof(tmpname), "plphp_temp_%u", funcoid);
	tmpsrc = (char *) palloc(strlen(prosrc) +
							 strlen(tmpname) +
							 strlen("function  ($args, $argc){ } "));
	sprintf(tmpsrc, "function %s($args, $argc){%s}",
			tmpname, prosrc);

	/*
	 * Delete the function from the PHP function table, just in case it
	 * already existed.  This is quite unlikely, but still.
	 */
	zend_hash_del(CG(function_table), tmpname, strlen(tmpname) + 1);

	/* Let the user see the fireworks */
	if (zend_eval_string(tmpsrc, NULL,
						 "plphp function temp source" TSRMLS_CC) == FAILURE)
		plphp_elog(ERROR, "function \"%s\" does not validate", funcname);

	pfree(tmpsrc);

	/* Delete the newly-created function from the PHP function table. */
	zend_hash_del(CG(function_table), tmpname, strlen(tmpname) + 1);

	/* The result of a validator is ignored */
	PG_RETURN_VOID();
}

/*
 * plphp_get_function_tupdesc
 *
 * 		Returns a TupleDesc of the function's return type.
 */
static TupleDesc
plphp_get_function_tupdesc(Oid result_type, ReturnSetInfo *rsinfo)
{
	if (result_type == RECORDOID)
	{
		/* We must get the information from call context */
		if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		return rsinfo->expectedDesc;
	}
	else
		/* ordinary composite type */
		return lookup_rowtype_tupdesc(result_type, -1);
}

static HeapTuple
plphp_modify_tuple(zval *pltd, TriggerData *tdata)
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
		elog(ERROR, "plphp: $_TD['new'] is not an array");

	plkeys = plphp_get_attr_name(plntup);
	natts = plphp_attr_count(plntup);

	if (natts != tupdesc->natts)
		elog(ERROR, "plphp: $_TD['new'] has an incorrect number of keys");

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
			elog(ERROR, "plphp: invalid attribute \"%s\" in tuple", platt);
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
		elog(ERROR, "plphp: SPI_modifytuple failed: %s",
			 SPI_result_code_string(SPI_result));

	return rtup;
}

/*
 * Build the $_TD array for the trigger function.
 */
static zval *
plphp_trig_build_args(FunctionCallInfo fcinfo)
{
	TriggerData	   *tdata;
	TupleDesc		tupdesc;
	zval		   *retval;
	int				i;

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	/* The basic variables */
	add_assoc_string(retval, "name", tdata->tg_trigger->tgname, 1);
	add_assoc_string(retval, "relid",
					 DatumGetCString(DirectFunctionCall1
									 (oidout,
									  ObjectIdGetDatum(tdata->tg_relation->rd_id))),
					 1);
	add_assoc_string(retval, "relname", SPI_getrelname(tdata->tg_relation), 1);

	/* EVENT */
	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
		add_assoc_string(retval, "event", "INSERT", 1);
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
		add_assoc_string(retval, "event", "DELETE", 1);
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		add_assoc_string(retval, "event", "UPDATE", 1);
	else
		elog(ERROR, "unknown firing event for trigger function");

	/* NEW and OLD as appropiate */
	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
	{
		if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			zend_hash_update(retval->value.ht, "new", strlen("new") + 1,
							 (void *) &hashref, sizeof(zval *), NULL);
		}
		else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			zend_hash_update(retval->value.ht, "old", strlen("old") + 1,
							 (void *) &hashref, sizeof(zval *), NULL);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_newtuple, tupdesc);
			zend_hash_update(retval->value.ht, "new", strlen("new") + 1,
							 (void *) &hashref, sizeof(zval *), NULL);

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			zend_hash_update(retval->value.ht, "old", strlen("old") + 1,
							 (void *) &hashref, sizeof(zval *), NULL);
		}
		else
			elog(ERROR, "unknown firing event for trigger function");
	}

	/* ARGC and ARGS */
	add_assoc_long(retval, "argc", tdata->tg_trigger->tgnargs);

	if (tdata->tg_trigger->tgnargs > 0)
	{
		zval	   *hashref;

		MAKE_STD_ZVAL(hashref);
		array_init(hashref);

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			add_index_string(hashref, i, tdata->tg_trigger->tgargs[i], 1);

		zend_hash_update(retval->value.ht, "args", strlen("args") + 1,
						 (void *) &hashref, sizeof(zval *), NULL);
	}

	/* WHEN */
	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		add_assoc_string(retval, "when", "BEFORE", 1);
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		add_assoc_string(retval, "when", "AFTER", 1);
	else
		elog(ERROR, "unknown firing time for trigger function");

	/* LEVEL */
	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		add_assoc_string(retval, "level", "ROW", 1);
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		add_assoc_string(retval, "level", "STATEMENT", 1);
	else
		elog(ERROR, "unknown firing level for trigger function");

	return retval;
}

/*
 * plphp_trigger_handler()		- Handler for trigger function calls
 */
static Datum
plphp_trigger_handler(FunctionCallInfo fcinfo)
{
	Datum		retval = 0;
	char	   *srv;
	zval	   *phpret,
			   *zTrigData;
	TriggerData *trigdata;
	plphp_proc_desc *desc;

	/*
	 * Find or compile the function
	 */
	desc = plphp_compile_function(fcinfo->flinfo->fn_oid, true);

	PG(safe_mode) = desc->lanpltrusted;

	zTrigData = plphp_trig_build_args(fcinfo);
	phpret = plphp_call_php_trig(desc, fcinfo, zTrigData);
	if (!phpret)
		elog(ERROR, "error during execution of function %s", desc->proname);

	/*
	 * Disconnect from SPI manager and then create the return values datum (if
	 * the input function does a palloc for it this must not be allocated in
	 * the SPI memory context because SPI_finish would free it).
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));


	trigdata = (TriggerData *) fcinfo->context;

	/*
	 * In a BEFORE trigger, compute the return value.  In an AFTER trigger
	 * it'll be ignored, so don't bother.
	 */
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
	{
		switch (phpret->type)
		{
			case IS_STRING:
				srv = phpret->value.str.val;
				if (strcasecmp(srv, "SKIP") == 0)
				{
					/* do nothing */
					break;
				}
				else if (strcasecmp(srv, "MODIFY") == 0)
				{
					if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
						retval = PointerGetDatum(plphp_modify_tuple(zTrigData,
																	trigdata));
					else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
						retval = PointerGetDatum(plphp_modify_tuple(zTrigData,
																	trigdata));
					else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
					{
						/* do nothing */;
						elog(WARNING,
							 "ignoring modified tuple in DELETE trigger");
					}
					else
						elog(ERROR, "unknown event in trigger function");
				}
				else
					plphp_elog(ERROR,
							   "expected trigger function to return NULL, "
							   "'SKIP' or 'MODIFY'");
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
				plphp_elog(ERROR,
						   "expected trigger function to return NULL, "
						   "'SKIP' or 'MODIFY'");
				break;
		}
	}

	return retval;
}

/*
 * plphp_func_handler()		- Handler for regular function calls
 */
static Datum
plphp_func_handler(FunctionCallInfo fcinfo)
{
	zval	   *phpret = NULL;
	plphp_proc_desc *desc;
	Datum		retval;
	char	   *retvalbuffer = NULL;

	/* Find or compile the function */
	desc = plphp_compile_function(fcinfo->flinfo->fn_oid, false);

	PG(safe_mode) = desc->lanpltrusted;

	/*
	 * Call the PHP function.  In a SRF, this is called multiple times; we must
	 * execute the function only the first time around -- the following
	 * iterations must extract tuples from the PHP array they are stored in.
	 */
	if (!(desc->ret_type & PL_SET))
		phpret = plphp_call_php_func(desc, fcinfo);
	else
	{
		if (SRF_IS_FIRSTCALL())
			srf_phpret = plphp_call_php_func(desc, fcinfo);
		phpret = srf_phpret;
	}

	/* Basic datatype checks */
	if ((desc->ret_type & PL_SET) && (phpret->type != IS_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return a set must return an array")));

	if ((desc->ret_type & PL_ARRAY) && (phpret->type != IS_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return array must return an array")));

	/* FIXME -- do we need to do the same for tuples? */

	/*
	 * Disconnect from SPI manager and then create the return values datum (if
	 * the input function does a palloc for it this must not be allocated in
	 * the SPI memory context because SPI_finish would free it).
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));
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
			/* XXX rewrite this stuff using StringInfo */
			case IS_LONG:
				retvalbuffer = (char *) palloc(256);
				snprintf(retvalbuffer, 256, "%ld", phpret->value.lval);
				break;
			case IS_DOUBLE:
				retvalbuffer = (char *) palloc(256);
				snprintf(retvalbuffer, 256, "%e", phpret->value.dval);
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
						td = plphp_get_function_tupdesc(desc->ret_oid,
									  					(ReturnSetInfo *)
														fcinfo->resultinfo);
					}
					else
						td = lookup_rowtype_tupdesc(desc->ret_oid, (int32) - 1);

					if (!td)
						plphp_elog(ERROR, "no TupleDesc info available");

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
					retval = plphp_srf_handler(fcinfo, desc);
				else
					elog(ERROR, "this plphp function cannot return arrays");
				break;
			case IS_NULL:
				fcinfo->isnull = true;
				break;
			default:
				elog(WARNING,
					 "plphp functions cannot return type %i",
					 phpret->type);
				fcinfo->isnull = true;
				break;
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
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl",
								  &query, &query_len, &limit) == FAILURE)
		{
			zend_error(E_WARNING, "Can not parse parameters in %s",
					   get_active_function_name(TSRMLS_C));
			return;
		}
	}
	else
	{
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s",
								  &query, &query_len) == FAILURE)
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

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a",
							  &param) == FAILURE)
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
		row = plphp_hash_from_tuple(tup_table->vals[counter],
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
 * plphp_compile_function
 *
 * 		Compile (or hopefully just look up) function
 */
static plphp_proc_desc *
plphp_compile_function(Oid fn_oid, int is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	plphp_proc_desc *prodesc = NULL;
	int			i;
	bool		uptodate;
	char	   *pointer = NULL;

	/*
	 * We'll need the pg_proc tuple in any case... 
	 */
	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Build our internal procedure name from the function's Oid
	 */
	if (is_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u_trigger", fn_oid);
	else
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u", fn_oid);

	/*
	 * Look up the internal proc name in the hashtable
	 */
	pointer = plphp_get_elem(plphp_proc_array, internal_proname);
	if (pointer)
	{
		sscanf(pointer, "%p", &prodesc);

		uptodate =
			(prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			 prodesc->fn_cmin == HeapTupleHeaderGetCmin(procTup->t_data));

		/* We need to delete the old entry */
		if (!uptodate)
		{
			/*
			 * FIXME -- use a per-function memory context and fix this
			 * stuff for good
			 */
			free(prodesc->proname);
			free(prodesc);
			prodesc = NULL;
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
		char	   *pointer = NULL;

		/*
		 * Allocate a new procedure description block
		 */
		prodesc = (plphp_proc_desc *) malloc(sizeof(plphp_proc_desc));
		if (!prodesc)
			plphp_elog(ERROR, "out of memory");

		MemSet(prodesc, 0, sizeof(plphp_proc_desc));
		prodesc->proname = strdup(internal_proname);
		if (!prodesc->proname)
			plphp_elog(ERROR, "out of memory");

		prodesc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		prodesc->fn_cmin = HeapTupleHeaderGetCmin(procTup->t_data);

		/*
		 * Look up the pg_language tuple by Oid
		 */
		langTup = SearchSysCache(LANGOID,
								 ObjectIdGetDatum(procStruct->prolang),
								 0, 0, 0);
		if (!HeapTupleIsValid(langTup))
		{
			free(prodesc->proname);
			free(prodesc);
			plphp_elog(ERROR, "cache lookup failed for language %u",
					   procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->lanpltrusted = langStruct->lanpltrusted;
		ReleaseSysCache(langTup);

		/*
		 * Get the required information for input conversion of the return
		 * value, and output conversion of the procedure's arguments.
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
					/* okay */
					prodesc->ret_type |= PL_PSEUDO;
				}
				else if (procStruct->prorettype == TRIGGEROID)
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("trigger functions may only be called "
									"as triggers")));
				}
				else
				{
					free(prodesc->proname);
					free(prodesc);
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("plphp functions cannot return type %s",
									format_type_be(procStruct->prorettype))));
				}
			}
			if (procStruct->proretset)
				prodesc->ret_type |= PL_SET;

			prodesc->ret_oid = procStruct->prorettype;

			if (typeStruct->typtype == 'c' ||
				procStruct->prorettype == RECORDOID)
			{
				prodesc->ret_type |= PL_TUPLE;
			}

			if (procStruct->prorettype == ANYARRAYOID)
				prodesc->ret_type |= PL_ARRAY;
			else
			{
				/* function returns a normal (declared) array */
				if (typeStruct->typlen == -1 && typeStruct->typelem)
					prodesc->ret_type |= PL_ARRAY;
			}

			perm_fmgr_info(typeStruct->typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = getTypeIOParam(typeTup);

			ReleaseSysCache(typeTup);

			prodesc->nargs = procStruct->pronargs;

			for (i = 0; i < prodesc->nargs; i++)
			{
#ifdef PG_VERSION_80_COMPAT
				Datum argid = procStruct->proargtypes[i];
#else
				Datum argid = procStruct->proargtypes.values[i];
#endif
				typeTup = SearchSysCache(TYPEOID, argid, 0, 0, 0);

				if (!HeapTupleIsValid(typeTup))
				{
					free(prodesc->proname);
					free(prodesc);
					plphp_elog(ERROR, "cache lookup failed for type %u",
							   DatumGetObjectId(argid));
				}

				typeStruct = (Form_pg_type) GETSTRUCT(typeTup);

				/* a pseudotype? */
				if (typeStruct->typtype == 'p')
					prodesc->arg_is_p[i] = true;
				else
					prodesc->arg_is_p[i] = false;

				/* deal with composite types */
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
		 * Create the text of the PHP function.  We do not use the same
		 * function name, because that would prevent function overloading.
		 * Sadly this also prevents PL/php functions from calling each other
		 * easily.
		 */
		prosrcdatum = SysCacheGetAttr(PROCOID, procTup,
									  Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			plphp_elog(ERROR, "cache lookup yielded NULL prosrc");

		proc_source = DatumGetCString(DirectFunctionCall1(textout,
														  prosrcdatum));

		/* Create the procedure in the interpreter */
		complete_proc_source =
			(char *) palloc(strlen(proc_source) +
							strlen(internal_proname) +
							strlen("function  ($args, $argc){ } "));

		/* XXX Is this usage of sprintf safe? */
		if (is_trigger)
			sprintf(complete_proc_source, "function %s($_TD){%s}",
					internal_proname, proc_source);
		else
			sprintf(complete_proc_source, "function %s($args, $argc){%s}",
					internal_proname, proc_source);

		zend_hash_del(CG(function_table), prodesc->proname,
					  strlen(prodesc->proname) + 1);

		pointer = (char *) palloc(64);
		sprintf(pointer, "%p", (void *) prodesc);
		add_assoc_string(plphp_proc_array, internal_proname,
						 (char *) pointer, 1);

		if (zend_eval_string(complete_proc_source, NULL,
							 "plphp function source" TSRMLS_CC) == FAILURE)
		{
			plphp_elog(ERROR, "unable to compile function \"%s\"",
					   prodesc->proname);
		}

		pfree(complete_proc_source);
	}

	ReleaseSysCache(procTup);

	return prodesc;
}

/*
 * plphp_func_build_args
 *
 * 		Build a PHP array representing the arguments to the function
 */
static zval *
plphp_func_build_args(plphp_proc_desc *desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	int			i;

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	if (!desc->nargs)
		return retval;

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
				HeapTupleHeader	td;
				Oid				tupType;
				int32			tupTypmod;
				TupleDesc		tupdesc;
				HeapTupleData	tmptup;
				zval		   *hashref;

				td = DatumGetHeapTupleHeader(fcinfo->arg[i]);

				/* Build a temporary HeapTuple control structure */
				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = DatumGetHeapTupleHeader(fcinfo->arg[i]);

				/* Extract rowtype info and find a tupdesc */
				tupType = HeapTupleHeaderGetTypeId(tmptup.t_data);
				tupTypmod = HeapTupleHeaderGetTypMod(tmptup.t_data);
				tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

				/* Build the PHP hash */
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
									 ObjectIdGetDatum(desc->arg_typioparam[i]),
									 Int32GetDatum(-1)));
				if (tmp[0] == '{')
				{
					zval	   *hashref;

					hashref = plphp_convert_from_pg_array(tmp);
					zend_hash_next_index_insert(retval->value.ht,
												(void *) &hashref,
												sizeof(zval *), NULL);
				}
				else
					add_next_index_string(retval, tmp, 1);

				/*
				 * FIXME - figure out which parameters are passed by
				 * reference and need freeing
				 */
				/* pfree(tmp); */
			}
		}
	}

	return retval;
}

/*
 * plphp_call_php_func
 *
 * 		Build the function argument array and call the PHP function.
 */
static zval *
plphp_call_php_func(plphp_proc_desc *desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	zval	   *args;
	zval	   *argc;
	char		call[64];

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	MAKE_STD_ZVAL(args);
	array_init(args);

	MAKE_STD_ZVAL(argc);
	ZVAL_LONG(argc, desc->nargs);

	args = plphp_func_build_args(desc, fcinfo);

	ZEND_SET_SYMBOL(&EG(symbol_table), "_args_int", args);
	ZEND_SET_SYMBOL(&EG(symbol_table), "_argc_int", argc);
	sprintf(call, "plphp_proc_%u($_args_int, $_argc_int);",
			fcinfo->flinfo->fn_oid);

	zend_try
	{
		if (zend_eval_string(call, retval, "plphp function call"
							 TSRMLS_CC) == FAILURE)
			plphp_elog(ERROR, "plphp: function call - failure");

	}
	zend_catch
	{
		plphp_elog(ERROR, "plphp: fatal error...");
	}
	zend_end_try();

	return retval;
}

/*
 * plphp_call_php_trig
 *
 * 		Build trigger argument array and call the PHP function as a
 * 		trigger.
 */
static zval *
plphp_call_php_trig(plphp_proc_desc *desc, FunctionCallInfo fcinfo,
					zval *trigdata)
{
	zval	   *retval;
	char		call[64];

	MAKE_STD_ZVAL(retval);
	retval->type = IS_NULL;

	ZEND_SET_SYMBOL(&EG(symbol_table), "_trigargs", trigdata);
	sprintf(call, "plphp_proc_%u_trigger(&$_trigargs);",
			fcinfo->flinfo->fn_oid);

	zend_try
	{
		if (zend_eval_string(call, retval, "plphp trigger call"
							 TSRMLS_CC) == FAILURE)
			plphp_elog(ERROR, "plphp: trigger call - failure");
	}
	zend_catch
	{
		plphp_elog(ERROR, "plphp: fatal error...");
	}
	zend_end_try();

	return retval;
}

/*
 * plphp_build_tuple_argument
 *
 * Build a PHP array from all attributes of a given tuple
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
		/* Ignore dropped attributes */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		/* Get the attribute name */
		attname = tupdesc->attrs[i]->attname.data;

		/* Get the attribute value */
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		/* If it is null, set it to undef in the hash. */
		if (isnull)
		{
			add_next_index_unset(output);
			continue;
		}

		/*
		 * Lookup the attribute type in the syscache for the output function
		 */
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

		/* Append the attribute name and the value to the list. */
		outputstr =
			DatumGetCString(OidFunctionCall3(typoutput, attr,
											 ObjectIdGetDatum(typioparam),
											 Int32GetDatum(tupdesc->attrs[i]->atttypmod)));
		add_assoc_string(output, attname, outputstr, 1);
		pfree(outputstr);
	}

	return output;
}

/*
 * Handle a SRF function.
 */
static Datum
plphp_srf_handler(FunctionCallInfo fcinfo, plphp_proc_desc *prodesc)
{
	FuncCallContext *funcctx;
	TupleDesc		tupdesc;
	TupleTableSlot *slot;
	AttInMetadata  *attinmeta = NULL;
	int				call_cntr;
	int				max_calls;
	char		  **values = NULL;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		if (srf_phpret->type != IS_ARRAY)
			plphp_elog(ERROR,
				   	   "this function must return a reference to an array");

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (prodesc->ret_type & PL_TUPLE)
		{
			/* Build a tupledesc */
			tupdesc =
				plphp_get_function_tupdesc(prodesc->ret_oid,
					  					   (ReturnSetInfo *) fcinfo->resultinfo);
			tupdesc = CreateTupleDescCopy(tupdesc);
			funcctx->tuple_desc = tupdesc;

			/* total number of tuples to be returned */
			funcctx->max_calls = plphp_get_rows_num(srf_phpret);

			/* allocate a slot for a tuple with this tupdesc */
			slot = TupleDescGetSlot(tupdesc);

			/* assign slot to function context */
			funcctx->slot = slot;

			/*
			 * generate attribute metadata needed later to produce tuples from
			 * raw C strings
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

	/* do when there is more left to send */
	if (call_cntr < max_calls)	
	{
		Datum		result;
		int			i;
		zval	   *zRow;

		if (prodesc->ret_type & PL_TUPLE)
		{
			HeapTuple	tuple;
			char	   *key;

			/*
			 * Prepare a values array for storage in our slot.  This should be
			 * an array of C strings which will be processed later by the type
			 * input functions.
			 */
			values = (char **) palloc(tupdesc->natts * sizeof(char *));
			zRow = plphp_get_row(srf_phpret, call_cntr);

			/* fill in each value */
			for (i = 0; i < tupdesc->natts; i++)
			{
				key = SPI_fname(tupdesc, i + 1);
				/* possibly NULL, but that's OK */
				values[i] = plphp_get_elem(zRow, key);
			}

			/* Build the tuple Datum */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			result = TupleGetDatum(slot, tuple);
		}
		else
		{
			char	   *retvalbuffer = NULL;

			/* FIXME -- we have to rewrite this, probably using StringInfo. */
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
					plphp_elog(ERROR, "plphp functions cannot return type %i",
							   zRow->type);
			}

			if (!fcinfo->isnull)
			{
				result = FunctionCall3(&prodesc->result_in_func,
									   PointerGetDatum(retvalbuffer),
									   ObjectIdGetDatum(prodesc->result_typioparam),
									   Int32GetDatum(-1));
				pfree(retvalbuffer);
			}
			else
				result = (Datum) 0;
		}

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

/*
 * pg_raise
 * 		User-callable function for sending messages to the Postgres log.
 */
ZEND_FUNCTION(pg_raise)
{
	char	   *level = NULL,
			   *message = NULL;
	int			level_len,
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

/*
 * plphp_error_cb
 *
 * A callback for PHP error handling.  This is called when the php_error or
 * zend_error function is invoked in our code.  Ideally this function should
 * clean up the PHP state after an ERROR, but zend_try blocks do not seem
 * to work as I'd expect.  So for now, we degrade the error to WARNING and 
 * continue executing in the hope that the system doesn't crash later.
 *
 * Note that we do clean up some PHP state by hand but it doesn't seem to
 * work as expected.
 */
void
plphp_error_cb(int type, const char *filename, const uint lineno,
					const char *fmt, va_list args)
{
	char	str[1024];
	int		elevel;

	vsnprintf(str, 1024, fmt, args);

	switch (type) {
		case E_ERROR:
		case E_CORE_ERROR:
		case E_COMPILE_ERROR:
		case E_USER_ERROR:
		case E_PARSE:
			elevel = ERROR;
			break;
		case E_WARNING:
		case E_CORE_WARNING:
		case E_COMPILE_WARNING:
		case E_USER_WARNING:
			elevel = WARNING;
			break;
		case E_NOTICE:
		case E_USER_NOTICE:
			elevel = NOTICE;
			break;
		default:
			elevel = ERROR;
			break;
	}

	/* XXX Temporary kludge while we get error trapping to work */
	if (elevel >= ERROR)
	{
		/* imitate _zend_bailout  */
		CG(unclean_shutdown) = 1;
		CG(in_compilation) = EG(in_execution) = 0;
		EG(current_execute_data) = NULL;

		elevel = WARNING;
	}

	plphp_elog(elevel, str);
}

/*
 * Final step in error reporting.  This function is supposed to throw
 * the error message to Postgres logs and then clean up the PHP state.
 * I still have to figure out how to make that last part work --
 * zend_try/zend_catch do not seem to work properly.
 */
void
plphp_elog(int elevel, char *fmt, ...)
{
	va_list		ap;
	char		msg[2048];

	va_start(ap, fmt);
	vsnprintf(msg, 2048, fmt, ap);
	va_end(ap);

	if (elevel >= ERROR)
	{
		/* imitate _zend_bailout  */
		CG(unclean_shutdown) = 1;
		CG(in_compilation) = EG(in_execution) = 0;
		EG(current_execute_data) = NULL;
	}

	ereport(elevel,
			(errmsg("plphp: %s", msg)));
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
