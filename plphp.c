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

/* PostgreSQL stuff */
#include "postgres.h"

/* This is defined in tcop/dest.h but PHP defines it again */
#define Debug DestDebug

#include "access/heapam.h"
#include "catalog/catversion.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"			/* needed for SRF support */
#include "lib/stringinfo.h"
#include "utils/builtins.h"
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
#undef Debug

/* PHP stuff */
#include "php.h"

#include "php_variables.h"
#include "php_globals.h"
#include "zend_hash.h"
#include "zend_modules.h"

#include "php_ini.h"			/* needed for INI_HARDCODED */
#include "php_main.h"

/* Our own stuff */
#include "plphp_io.h"
#include "plphp_spi.h"

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


#undef DEBUG_PLPHP_MEMORY

#ifdef DEBUG_PLPHP_MEMORY
#define REPORT_PHP_MEMUSAGE(where) \
	elog(NOTICE, "PHP mem usage: �%s�: %u", where, AG(allocated_memory));
#else
#define REPORT_PHP_MEMUSAGE(a) 
#endif

/*
 * These symbols are needed by the Apache module (libphp*.so).  We don't
 * actually use those, but they are needed so that the linker can do its
 * job without complaining.
 *
 * FIXME -- this looks like a mighty hacky way of doing things.  Apparently
 * a more correct way of doing it would be creating a new PHP SAPI, but I'm
 * refraining from that, at least for now.
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

/* Used in PHP 5.1.1 */
void	   *apr_pool_cleanup_run = NULL;
void	   *apr_snprintf = NULL;

/*
 * Return types.  Why on earth is this a bitmask?  Beats me.
 * We should have separate flags instead.
 */
typedef enum pl_type
{
	PL_TUPLE = 1 << 0,
	PL_ARRAY = 1 << 1,
	PL_PSEUDO = 1 << 2
} pl_type;

/*
 * The information we cache about loaded procedures.
 *
 * "proname" is the name of the function, given by the user.
 *
 * fn_xmin and fn_xmin are used to know when a function has been redefined and
 * needs to be recompiled.
 *
 * trusted indicates whether the function was created with a trusted handler.
 *
 * ret_type is a weird bitmask that indicates whether this function returns a
 * tuple, an array or a pseudotype.  ret_oid is the Oid of the return type.
 * retset indicates whether the function was declared to return a set.
 *
 * XXX -- maybe this thing needs to be rethought.
 */
typedef struct plphp_proc_desc
{
	char	   *proname;
	TransactionId fn_xmin;
	CommandId	fn_cmin;
	bool		trusted;
	pl_type		ret_type;
	Oid			ret_oid;		/* Oid of returning type */
	bool		retset;
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

/* for PHP write/flush */
static StringInfo currmsg = NULL;

/*
 * for PHP <-> Postgres error message passing
 *
 * XXX -- it would be much better if we could save errcontext,
 * errhint, etc as well.
 */
static char *error_msg = NULL;

/*
 * Forward declarations
 */
static void plphp_init_all(void);
void		plphp_init(void);

PG_FUNCTION_INFO_V1(plphp_call_handler);
Datum plphp_call_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plphp_validator);
Datum plphp_validator(PG_FUNCTION_ARGS);

static Datum plphp_trigger_handler(FunctionCallInfo fcinfo,
								   plphp_proc_desc *desc);
static Datum plphp_func_handler(FunctionCallInfo fcinfo,
								plphp_proc_desc *desc);
static Datum plphp_srf_handler(FunctionCallInfo fcinfo,
						   	   plphp_proc_desc *desc);

static plphp_proc_desc *plphp_compile_function(Oid fnoid, bool is_trigger);
static zval *plphp_call_php_func(plphp_proc_desc *desc,
								 FunctionCallInfo fcinfo);
static zval *plphp_call_php_trig(plphp_proc_desc *desc,
								 FunctionCallInfo fcinfo, zval *trigdata);

static void plphp_error_cb(int type, const char *filename, const uint lineno,
								  const char *fmt, va_list args);

/*
 * FIXME -- this comment is quite misleading actually, which is not surprising
 * since it came verbatim from PL/pgSQL.  Rewrite memory handling here someday
 * and remove it.
 *
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

/*
 * sapi_plphp_write
 * 		Called when PHP wants to write something to stdout.
 *
 * We just save the output in a StringInfo until the next Flush call.
 */
static int
sapi_plphp_write(const char *str, uint str_length TSRMLS_DC)
{
	if (currmsg == NULL)
		currmsg = makeStringInfo();

	appendStringInfoString(currmsg, str);

	return str_length;
}

/*
 * sapi_plphp_flush
 * 		Called when PHP wants to flush stdout.
 *
 * The stupid PHP implementation calls write and follows with a Flush right
 * away -- a good implementation would write several times and flush when the
 * message is complete.  To make the output look reasonable in Postgres, we
 * skip the flushing if the accumulated message does not end in a newline.
 */
static void
sapi_plphp_flush(void *sth)
{
	if (currmsg != NULL)
	{
		if (currmsg->data[currmsg->len - 1] == '\n')
		{
			Assert(currmsg->data != NULL);

			/*
			 * remove the trailing newline because elog() inserts another
			 * one
			 */
			currmsg->data[currmsg->len - 1] = '\0';
			elog(LOG, "%s", currmsg->data);

			pfree(currmsg->data);
			pfree(currmsg);
			currmsg = NULL;
		}
	}
	else
		elog(LOG, "attempting to flush a NULL message");
}

static int
sapi_plphp_send_headers(TSRMLS_CC)
{
	return 1;
}

static void
php_plphp_log_messages(char *message)
{
	elog(LOG, "plphp: %s", message);
}


static sapi_module_struct plphp_sapi_module = {
	"plphp",					/* name */
	"PL/php PostgreSQL Handler",/* pretty name */

	NULL,						/* startup */
	php_module_shutdown_wrapper,/* shutdown */

	NULL,						/* activate */
	NULL,						/* deactivate */

	sapi_plphp_write,			/* unbuffered write */
	sapi_plphp_flush,			/* flush */
	NULL,						/* stat */
	NULL,						/* getenv */

	php_error,					/* sapi_error(int, const char *, ...) */

	NULL,						/* header handler */
	sapi_plphp_send_headers,	/* send headers */
	NULL,						/* send header */

	NULL,						/* read POST */
	NULL,						/* read cookies */

	NULL,						/* register server variables */
	php_plphp_log_messages,		/* log message */

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

	/*
	 * Need a Pg try/catch block to prevent an initialization-
	 * failure from bringing the whole server down.
	 */
	PG_TRY();
	{
		zend_try
		{
			/*
			 * XXX This is a hack -- we are replacing the error callback in an
			 * invasive manner that should not be expected to work on future PHP
			 * releases.
			 */
			zend_error_cb = plphp_error_cb;

			/* Omit HTML tags from output */
			plphp_sapi_module.phpinfo_as_text = 1;
			sapi_startup(&plphp_sapi_module);

			if (php_module_startup(&plphp_sapi_module, NULL, 0) == FAILURE)
				elog(ERROR, "php_module_startup call failed");

			/* php_module_startup changed it, so put it back */
			zend_error_cb = plphp_error_cb;

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

			zend_register_functions(
#if PHP_MAJOR_VERSION == 5
									NULL,
#endif
									spi_functions, NULL,
									MODULE_PERSISTENT TSRMLS_CC);

			PG(during_request_startup) = true;

			/* Set some defaults */
			SG(options) |= SAPI_OPTION_NO_CHDIR;

			/* Hard coded defaults which cannot be overwritten in the ini file */
			INI_HARDCODED("register_argc_argv", "0");
			INI_HARDCODED("html_errors", "0");
			INI_HARDCODED("implicit_flush", "1");
			INI_HARDCODED("max_execution_time", "0");

			/*
			 * Set memory limit to ridiculously high value.  This helps the
			 * server not to crash, because the PHP allocator has the really
			 * stupid idea of calling exit() if the limit is exceeded.
			 */
			{
				char	limit[15];

				snprintf(limit, sizeof(limit), "%d", 1 << 30);
				INI_HARDCODED("memory_limit", limit);
			}

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
				elog(ERROR, "php_request_startup call failed");
			}

			CG(interactive) = true;
			PG(during_request_startup) = true;

			/* Register the resource for SPI_result */
			SPIres_rtype = zend_register_list_destructors_ex(php_SPIresult_destroy,
															 NULL, 
															 "SPI result",
															 0);

			/* Ok, we're done */
			plphp_first_call = false;
		}
		zend_catch
		{
			plphp_first_call = true;
			if (error_msg)
			{
				char	str[1024];

				strncpy(str, error_msg, sizeof(str));
				pfree(error_msg);
				error_msg = NULL;
				elog(ERROR, "fatal error during PL/php initialization: %s",
					 str);
			}
			else
				elog(ERROR, "fatal error during PL/php initialization");
		}
		zend_end_try();
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();
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

	PG_TRY();
	{
		/* Connect to SPI manager */
		if (SPI_connect() != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to SPI manager")));

		zend_try
		{
			plphp_proc_desc *desc;

			/* Clean up SRF state */
			current_fcinfo = NULL;

			/* Redirect to the appropiate handler */
			if (CALLED_AS_TRIGGER(fcinfo))
			{
				desc = plphp_compile_function(fcinfo->flinfo->fn_oid, true);

				/* Activate PHP safe mode if needed */
				PG(safe_mode) = desc->trusted;

				retval = plphp_trigger_handler(fcinfo, desc);
			}
			else
			{
				desc = plphp_compile_function(fcinfo->flinfo->fn_oid, false);

				/* Activate PHP safe mode if needed */
				PG(safe_mode) = desc->trusted;

				if (desc->retset)
					retval = plphp_srf_handler(fcinfo, desc);
				else
					retval = plphp_func_handler(fcinfo, desc);
			}
		}
		zend_catch
		{
			REPORT_PHP_MEMUSAGE("reporting error");
			if (error_msg)
			{
				char	str[1024];

				strncpy(str, error_msg, sizeof(str));
				pfree(error_msg);
				error_msg = NULL;
				elog(ERROR, "%s", str);
			}
			else
				elog(ERROR, "fatal error");

			/* not reached, but keep compiler quiet */
			return 0;
		}
		zend_end_try();
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();

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
	char		   *tmpsrc = NULL,
				   *prosrc;
	Datum			prosrcdatum;
	bool			isnull;

	/* Initialize interpreter */
	plphp_init_all();

	PG_TRY();
	{
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
			elog(ERROR, "cache lookup yielded NULL prosrc");
		prosrc = DatumGetCString(DirectFunctionCall1(textout,
													 prosrcdatum));

		/* Get the function name, for the error message */
		StrNCpy(funcname, NameStr(procForm->proname), NAMEDATALEN);

		/* Let go of the pg_proc tuple */
		ReleaseSysCache(procTup);

		/* Create a PHP function creation statement */
		snprintf(tmpname, sizeof(tmpname), "plphp_temp_%u", funcoid);
		tmpsrc = (char *) palloc(strlen(prosrc) +
								 strlen(tmpname) +
								 strlen("function  ($args, $argc){ } "));
		sprintf(tmpsrc, "function %s($args, $argc){%s}",
				tmpname, prosrc);

		pfree(prosrc);

		zend_try
		{
			/*
			 * Delete the function from the PHP function table, just in case it
			 * already existed.  This is quite unlikely, but still.
			 */
			zend_hash_del(CG(function_table), tmpname, strlen(tmpname) + 1);

			/*
			 * Let the user see the fireworks.  If the function doesn't validate,
			 * the ERROR will be raised and the function will not be created.
			 */
			if (zend_eval_string(tmpsrc, NULL,
								 "plphp function temp source" TSRMLS_CC) == FAILURE)
				elog(ERROR, "function \"%s\" does not validate", funcname);

			pfree(tmpsrc);
			tmpsrc = NULL;

			/* Delete the newly-created function from the PHP function table. */
			zend_hash_del(CG(function_table), tmpname, strlen(tmpname) + 1);
		}
		zend_catch
		{
			if (tmpsrc != NULL)
				pfree(tmpsrc);

			if (error_msg)
			{
				char	str[1024];

				StrNCpy(str, error_msg, sizeof(str));
				pfree(error_msg);
				error_msg = NULL;
				elog(ERROR, "function \"%s\" does not validate: %s", funcname, str);
			}
			else
				elog(ERROR, "fatal error");

			/* not reached, but keep compiler quiet */
			return 0;
		}
		zend_end_try();

		/* The result of a validator is ignored */
		PG_RETURN_VOID();
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * plphp_get_function_tupdesc
 *
 * 		Returns a TupleDesc of the function's return type.
 */
static TupleDesc
plphp_get_function_tupdesc(Oid result_type, Node *rsinfo)
{
	if (result_type == RECORDOID)
	{
		ReturnSetInfo *rs = (ReturnSetInfo *) rsinfo;
		/* We must get the information from call context */
		if (!rsinfo || !IsA(rsinfo, ReturnSetInfo) || rs->expectedDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		return rs->expectedDesc;
	}
	else
		/* ordinary composite type */
		return lookup_rowtype_tupdesc(result_type, -1);
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
    add_assoc_long(retval, "relid", tdata->tg_relation->rd_id);
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
 * plphp_trigger_handler
 * 		Handler for trigger function calls
 */
static Datum
plphp_trigger_handler(FunctionCallInfo fcinfo, plphp_proc_desc *desc)
{
	Datum		retval = 0;
	char	   *srv;
	zval	   *phpret,
			   *zTrigData;
	TriggerData *trigdata;

	REPORT_PHP_MEMUSAGE("going to build the trigger arg");

	zTrigData = plphp_trig_build_args(fcinfo);

	REPORT_PHP_MEMUSAGE("going to call the trigger function");

	phpret = plphp_call_php_trig(desc, fcinfo, zTrigData);
	if (!phpret)
		elog(ERROR, "error during execution of function %s", desc->proname);

	REPORT_PHP_MEMUSAGE("trigger called, going to build the return value");

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

	if (zTrigData->type != IS_ARRAY)
		elog(ERROR, "$_TD is not an array");
			 
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
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("on delete trigger can not modify the "
										"the return tuple")));
					else
						elog(ERROR, "unknown event in trigger function");
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("expected trigger function to return "
									"NULL, 'SKIP' or 'MODIFY'")));
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
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("expected trigger function to return "
								"NULL, 'SKIP' or 'MODIFY'")));
				break;
		}
	}

	REPORT_PHP_MEMUSAGE("freeing some variables");

	zval_dtor(zTrigData);
	zval_dtor(phpret);

	FREE_ZVAL(phpret);
	FREE_ZVAL(zTrigData);

	REPORT_PHP_MEMUSAGE("trigger call done");

	return retval;
}

/*
 * plphp_func_handler
 * 		Handler for regular function calls
 */
static Datum
plphp_func_handler(FunctionCallInfo fcinfo, plphp_proc_desc *desc)
{
	zval	   *phpret = NULL;
	Datum		retval;
	char	   *retvalbuffer = NULL;

	/* SRFs are handled separately */
	Assert(!desc->retset);

	/* Call the PHP function.  */
	phpret = plphp_call_php_func(desc, fcinfo);

	REPORT_PHP_MEMUSAGE("function invoked");

	/* Basic datatype checks */
	if ((desc->ret_type & PL_ARRAY) && (phpret->type != IS_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return array must return an array")));
	if ((desc->ret_type & PL_TUPLE) && (phpret->type != IS_ARRAY))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return tuple must return an array")));

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
			case IS_NULL:
				fcinfo->isnull = true;
				break;
			case IS_LONG:
			case IS_DOUBLE:
			case IS_STRING:
				retvalbuffer = plphp_zval_get_cstring(phpret, false, false);
				retval = CStringGetDatum(retvalbuffer);
				break;
			case IS_ARRAY:
				if (desc->ret_type & PL_ARRAY)
				{
					retvalbuffer = plphp_convert_to_pg_array(phpret);
					retval = CStringGetDatum(retvalbuffer);
				}
				else if (desc->ret_type & PL_TUPLE)
				{
					TupleDesc	td;
					HeapTuple	tup;

					if (desc->ret_type & PL_PSEUDO)
						td = plphp_get_function_tupdesc(desc->ret_oid,
														fcinfo->resultinfo);
					else
						td = lookup_rowtype_tupdesc(desc->ret_oid, (int32) -1);

					if (!td)
						elog(ERROR, "no TupleDesc info available");

					tup = plphp_htup_from_zval(phpret, td);
					retval = HeapTupleGetDatum(tup);
				}
				else
					/* FIXME -- should return the thing as a string? */
					elog(ERROR, "this plphp function cannot return arrays");
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

	if (!fcinfo->isnull && !(desc->ret_type & PL_TUPLE))
	{
		retval = FunctionCall3(&desc->result_in_func,
							   PointerGetDatum(retvalbuffer),
							   ObjectIdGetDatum(desc->result_typioparam),
							   Int32GetDatum(-1));
		pfree(retvalbuffer);
	}

	REPORT_PHP_MEMUSAGE("finished calling user function");

	return retval;
}

/*
 * plphp_srf_handler
 * 		Invoke a SRF
 */
static Datum
plphp_srf_handler(FunctionCallInfo fcinfo, plphp_proc_desc *desc)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	zval	   *phpret;
	MemoryContext	oldcxt;

	Assert(desc->retset);

	current_fcinfo = fcinfo;
	current_tuplestore = NULL;

	/* Check context before allowing the call to go through */
	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	/*
	 * Fetch the function's tuple descriptor.  This will return NULL in the
	 * case of a scalar return type, in which case we will copy the TupleDesc
	 * from the ReturnSetInfo.
	 */
	get_call_result_type(fcinfo, NULL, &tupdesc);
	if (tupdesc == NULL)
		tupdesc = rsi->expectedDesc;

	/*
	 * If the expectedDesc is NULL, bail out, because most likely it's using
	 * IN/OUT parameters.
	 */
	if (tupdesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use IN/OUT parameters in PL/php")));

	oldcxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	/* This context is reset once per row in return_next */
	current_memcxt = AllocSetContextCreate(CurTransactionContext,
										   "PL/php SRF context",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	/* Tuple descriptor and AttInMetadata for return_next */
	current_tupledesc = CreateTupleDescCopy(tupdesc);
	current_attinmeta = TupleDescGetAttInMetadata(current_tupledesc);

	/*
	 * Call the PHP function.  The user code must call return_next, which will
	 * create and populate the tuplestore appropiately.
	 */
	phpret = plphp_call_php_func(desc, fcinfo);

	/* We don't use the return value */
	zval_dtor(phpret);

	/* Close the SPI connection */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	/* Now prepare the return values. */
	rsi->returnMode = SFRM_Materialize;

	if (current_tuplestore)
	{
		rsi->setResult = current_tuplestore;
		rsi->setDesc = current_tupledesc;
	}

	MemoryContextDelete(current_memcxt);
	current_memcxt = NULL;
	current_tupledesc = NULL;
	current_attinmeta = NULL;

	MemoryContextSwitchTo(oldcxt);

	/* All done */
	return (Datum) 0;
}

/*
 * plphp_compile_function
 *
 * 		Compile (or hopefully just look up) function
 */
static plphp_proc_desc *
plphp_compile_function(Oid fnoid, bool is_trigger)
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
	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fnoid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fnoid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Build our internal procedure name from the function's Oid
	 */
	if (is_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u_trigger", fnoid);
	else
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u", fnoid);

	/*
	 * Look up the internal proc name in the hashtable
	 */
	pointer = plphp_zval_get_cstring(plphp_array_get_elem(plphp_proc_array,
														  internal_proname),
									 false, true);
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
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		MemSet(prodesc, 0, sizeof(plphp_proc_desc));
		prodesc->proname = strdup(internal_proname);
		if (!prodesc->proname)
		{
			free(prodesc);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

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
			elog(ERROR, "cache lookup failed for language %u",
					   procStruct->prolang);
		}
		langStruct = (Form_pg_language) GETSTRUCT(langTup);
		prodesc->trusted = langStruct->lanpltrusted;
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

			prodesc->ret_oid = procStruct->prorettype;
			prodesc->retset = procStruct->proretset;

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
					elog(ERROR, "cache lookup failed for type %u",
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
			elog(ERROR, "cache lookup yielded NULL prosrc");

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
			/* the next compilation will blow it up */
			prodesc->fn_xmin = InvalidTransactionId;
			elog(ERROR, "unable to compile function \"%s\"",
					   prodesc->proname);
		}

		pfree(complete_proc_source);
	}

	ReleaseSysCache(procTup);

	return prodesc;
}

/*
 * plphp_func_build_args
 * 		Build a PHP array representing the arguments to the function
 */
static zval *
plphp_func_build_args(plphp_proc_desc *desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	int			i;

	MAKE_STD_ZVAL(retval);
	array_init(retval);

	if (desc->nargs == 0)
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
				/*
				 * FIXME -- this is bogus.  Not every value starting with { is
				 * an array.  Figure out a better method for detecting arrays.
				 */
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
 * 		Build the function argument array and call the PHP function.
 *
 * We use a private PHP symbol table, so that we can easily destroy everything
 * used during the execution of the function.  We use it to collect the
 * arguments' zvals as well.  We exclude the return value, because it will be
 * used by the caller -- it must be freed there!
 */
static zval *
plphp_call_php_func(plphp_proc_desc *desc, FunctionCallInfo fcinfo)
{
	zval	   *retval;
	zval	   *args;
	zval	   *argc;
	zval	   *funcname;
	zval	  **params[2];
	char		call[64];
	HashTable  *orig_symbol_table;
	HashTable  *symbol_table;

	REPORT_PHP_MEMUSAGE("going to build function args");

	ALLOC_HASHTABLE(symbol_table);
	zend_hash_init(symbol_table, 0, NULL, ZVAL_PTR_DTOR, 0);

	/*
	 * Build the function arguments.  Save a pointer to each new zval in our
	 * private symbol table, so that we can clean up easily later.
	 */
	args = plphp_func_build_args(desc, fcinfo);
	zend_hash_update(symbol_table, "args", strlen("args") + 1,
					 (void *) &args, sizeof(zval *), NULL);

	REPORT_PHP_MEMUSAGE("args built. Now the rest ...");

	MAKE_STD_ZVAL(argc);
	ZVAL_LONG(argc, desc->nargs);
	zend_hash_update(symbol_table, "argc", strlen("argc") + 1,
					 (void *) &args, sizeof(zval *), NULL);

	params[0] = &args;
	params[1] = &argc;

	/* Build the internal function name, and save for later cleaning */
	sprintf(call, "plphp_proc_%u", fcinfo->flinfo->fn_oid);
	MAKE_STD_ZVAL(funcname);
	ZVAL_STRING(funcname, call, 1);
	zend_hash_update(symbol_table, "funcname", strlen("funcname") + 1,
					 (void *) &args, sizeof(zval *), NULL);

	REPORT_PHP_MEMUSAGE("going to call the function");

	orig_symbol_table = EG(active_symbol_table);
	EG(active_symbol_table) = symbol_table;

	if (call_user_function_ex(EG(function_table), NULL, funcname, &retval,
							  2, params, 1, NULL TSRMLS_CC) == FAILURE)
		elog(ERROR, "could not call function \"%s\"", call);

	REPORT_PHP_MEMUSAGE("going to free some vars");

	/* Return to the original symbol table, and clean our private one */
	EG(active_symbol_table) = orig_symbol_table;
	zend_hash_clean(symbol_table);

	REPORT_PHP_MEMUSAGE("function call done");

	return retval;
}

/*
 * plphp_call_php_trig
 * 		Build trigger argument array and call the PHP function as a
 * 		trigger.
 *
 * Note we don't need to change the symbol table here like we do in
 * plphp_call_php_func, because we do manual cleaning of each zval used.
 */
static zval *
plphp_call_php_trig(plphp_proc_desc *desc, FunctionCallInfo fcinfo,
					zval *trigdata)
{
	zval	   *retval;
	zval	   *funcname;
	char		call[64];
	zval	  **params[1];

	params[0] = &trigdata;

	/* Build the internal function name, and save for later cleaning */
	sprintf(call, "plphp_proc_%u_trigger", fcinfo->flinfo->fn_oid);
	MAKE_STD_ZVAL(funcname);
	ZVAL_STRING(funcname, call, 0);

	/*
	 * HACK: mark trigdata as a reference, so it won't be copied in
	 * call_user_function_ex.  This way the user function will be able to 
	 * modify it, in order to change NEW.
	 */
	trigdata->is_ref = 1;

	if (call_user_function_ex(EG(function_table), NULL, funcname, &retval,
							  1, params, 1, NULL TSRMLS_CC) == FAILURE)
		elog(ERROR, "could not call function \"%s\"", call);

	FREE_ZVAL(funcname);

	/* Return to the original state */
	trigdata->is_ref = 0;

	return retval;
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
 * work as expected either.
 */
void
plphp_error_cb(int type, const char *filename, const uint lineno,
	   		   const char *fmt, va_list args)
{
	char	str[1024];
	int		elevel;

	vsnprintf(str, 1024, fmt, args);

	/*
	 * PHP error classification is a bitmask, so this conversion is a bit
	 * bogus.  However, most calls to php_error() use a single bit.
	 * Whenever more than one is used, we will default to ERROR, so this is
	 * safe, if a bit excessive.
	 *
	 * XXX -- I wonder whether we should promote the WARNINGs to errors as
	 * well.  PHP has a really stupid way of continuing execution in presence
	 * of severe problems that I don't see why we should maintain.
	 */
	switch (type)
	{
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

	REPORT_PHP_MEMUSAGE("reporting error");

	/*
	 * If this is a severe problem, we need to make PHP aware of it, so first
	 * save the error message and then bail out of the PHP block.  With luck,
	 * this will be trapped by a zend_try/zend_catch block outwards in PL/php
	 * code, which would translate it to a Postgres elog(ERROR), leaving
	 * everything in a consistent state.
	 *
	 * For this to work, there must be a try/catch block covering every place
	 * where PHP may raise an error!
	 */
	if (elevel >= ERROR)
	{
		if (lineno != 0)
		{
			char	msgline[1024];
			snprintf(msgline, sizeof(msgline), "%s at line %d", str, lineno);
			error_msg = pstrdup(msgline);
		}
		else
			error_msg = pstrdup(str);

		zend_bailout();
	}

	ereport(elevel,
			(errmsg("plphp: %s", str)));
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
