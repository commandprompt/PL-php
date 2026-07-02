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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/transam.h"

#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"

#include "commands/event_trigger.h"	/* event trigger support */
#include "commands/trigger.h"
#include "fmgr.h"
#include "funcapi.h"			/* needed for SRF support */
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"	/* InlineCodeBlock, for DO blocks */
#if PG_VERSION_NUM >= 130000
#include "tcop/cmdtag.h"		/* GetCommandTagName, for event triggers */
#endif
#include "utils/guc.h"			/* plphp.start_proc GUC */

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
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
#include "zend_exceptions.h"	/* zend_clear_exception / zend_read_property */

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

#define INI_HARDCODED(name, value) \
	do { \
		zend_string *_ini_name = zend_string_init((name), sizeof(name) - 1, 0); \
		zend_alter_ini_entry_chars(_ini_name, (value), strlen(value), \
								   PHP_INI_SYSTEM, PHP_INI_STAGE_ACTIVATE); \
		zend_string_release(_ini_name); \
	} while (0)

#undef DEBUG_PLPHP_MEMORY

#ifdef DEBUG_PLPHP_MEMORY
#define REPORT_PHP_MEMUSAGE(where) \
	elog(NOTICE, "PHP mem usage: %s: %u", where, AG(allocated_memory));
#else
#define REPORT_PHP_MEMUSAGE(a) 
#endif

PG_MODULE_MAGIC;

/*
 * FunctionCallInfo argument access.  PostgreSQL 12 replaced the fixed arg[]/
 * argnull[] arrays with a flexible NullableDatum args[] array.
 */
#if PG_VERSION_NUM >= 120000
#define PLPHP_ARG_ISNULL(fcinfo, n)	((fcinfo)->args[n].isnull)
#define PLPHP_ARG_VALUE(fcinfo, n)	((fcinfo)->args[n].value)
#else
#define PLPHP_ARG_ISNULL(fcinfo, n)	((fcinfo)->argnull[n])
#define PLPHP_ARG_VALUE(fcinfo, n)	((fcinfo)->arg[n])
#endif

/* Check the argument type to expect to accept an initial value */
#define IS_ARGMODE_OUT(mode) ((mode) == PROARGMODE_OUT || \
(mode) == PROARGMODE_TABLE)
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
 * fn_xmin and fn_cmin are used to know when a function has been redefined and
 * needs to be recompiled.
 *
 * trusted indicates whether the function was created with a trusted handler.
 *
 * ret_type is a weird bitmask that indicates whether this function returns a
 * tuple, an array or a pseudotype.  ret_oid is the Oid of the return type.
 * retset indicates whether the function was declared to return a set.
 *
 * arg_argmode indicates whether the argument is IN, OUT or both. It follows
 * values in pg_proc.proargmodes.
 *
 * n_out_args - total number of OUT or INOUT arguments.
 * arg_out_tupdesc is a tuple descriptor of the tuple constructed for OUT args.
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
	int			n_out_args;
	int			n_total_args;
	int			n_mixed_args;
	FmgrInfo	arg_out_func[FUNC_MAX_ARGS];
	Oid			arg_typioparam[FUNC_MAX_ARGS];
	char		arg_typtype[FUNC_MAX_ARGS];
	char		arg_argmode[FUNC_MAX_ARGS];
	TupleDesc	args_out_tupdesc;
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

/* GUC: name of a function to run once when the interpreter is initialized */
static char *plphp_start_proc = NULL;

/* Has the per-session init (module loading + start_proc) run yet? */
static bool plphp_session_inited = false;

/*
 * Forward declarations
 */
void		_PG_init(void);
static void plphp_init_all(void);
void		plphp_init(void);
static void plphp_session_init(void);
static void plphp_load_modules(void);

PG_FUNCTION_INFO_V1(plphp_call_handler);
Datum plphp_call_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plphp_validator);
Datum plphp_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plphp_inline_handler);
Datum plphp_inline_handler(PG_FUNCTION_ARGS);

static Datum plphp_trigger_handler(FunctionCallInfo fcinfo,
								   plphp_proc_desc *desc);
static Datum plphp_event_trigger_handler(FunctionCallInfo fcinfo,
										 plphp_proc_desc *desc);
static Datum plphp_func_handler(FunctionCallInfo fcinfo,
							    plphp_proc_desc *desc);
static Datum plphp_srf_handler(FunctionCallInfo fcinfo,
						   	   plphp_proc_desc *desc);

static plphp_proc_desc *plphp_compile_function(Oid fnoid, bool is_trigger,
											   bool is_event_trigger);
static zval *plphp_call_php_func(plphp_proc_desc *desc,
								 FunctionCallInfo fcinfo);
static zval *plphp_call_php_trig(plphp_proc_desc *desc,
								 FunctionCallInfo fcinfo, zval *trigdata);

static void plphp_error_cb(int type, zend_string *error_filename,
						   const uint32_t error_lineno, zend_string *message);
static bool is_valid_php_identifier(char *name);
static char *plphp_pop_exception_message(void);

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
static size_t
sapi_plphp_write(const char *str, size_t str_length)
{
	if (currmsg == NULL)
		currmsg = makeStringInfo();

	appendBinaryStringInfo(currmsg, str, str_length);

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
		Assert(currmsg->data != NULL);

		if (currmsg->data[currmsg->len - 1] == '\n')
		{
			/*
			 * remove the trailing newline because elog() inserts another
			 * one
			 */
			currmsg->data[currmsg->len - 1] = '\0';
		}
		elog(LOG, "%s", currmsg->data);

		pfree(currmsg->data);
		pfree(currmsg);
		currmsg = NULL;
	}
	else
		elog(LOG, "attempting to flush a NULL message");
}

static int
sapi_plphp_send_headers(sapi_headers_struct *sapi_headers)
{
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void
php_plphp_log_messages(const char *message, int syslog_type_int)
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
 * _PG_init
 * 		Module-load callback: register configuration variables.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("plphp.start_proc",
							   "PL/php function to call when the interpreter "
							   "is first initialized in a session.",
							   NULL,
							   &plphp_start_proc,
							   NULL,
							   PGC_SUSET, 0,
							   NULL, NULL, NULL);
#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("plphp");
#else
	EmitWarningsOnPlaceholders("plphp");
#endif
}

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

			if (php_module_startup(&plphp_sapi_module, NULL) == FAILURE)
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

			/* Set some defaults */
			SG(options) |= SAPI_OPTION_NO_CHDIR;

			/* Hard coded defaults which cannot be overwritten in the ini file */
			INI_HARDCODED("register_argc_argv", "0");
			INI_HARDCODED("html_errors", "0");
			INI_HARDCODED("implicit_flush", "1");
			INI_HARDCODED("max_execution_time", "0");
			INI_HARDCODED("max_input_time", "-1");

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

			if (php_request_startup() == FAILURE)
			{
				SG(headers_sent) = 1;
				SG(request_info).no_headers = 1;
				/* Use Postgres log */
				elog(ERROR, "php_request_startup call failed");
			}

			PG(during_request_startup) = true;

			/*
			 * Register our SPI functions and the procedure cache now that the
			 * request (and its interned-string tables) is fully initialized.
			 */
			zend_register_functions(NULL, spi_functions, NULL,
									MODULE_PERSISTENT);

			/* Init procedure cache */
			plphp_proc_array = (zval *) emalloc(sizeof(zval));
			array_init(plphp_proc_array);

			/* Register the resource for SPI_result */
			SPIres_rtype = zend_register_list_destructors_ex(php_SPIresult_destroy,
															 NULL,
															 "SPI result",
															 0);

			/* Register the resource for prepared plans */
			SPIplan_rtype = zend_register_list_destructors_ex(php_SPIplan_destroy,
															  NULL,
															  "plphp SPI plan",
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
 * plphp_load_modules
 * 		If a table named plphp_modules exists, load each row's PHP source so
 * 		that any functions/classes it defines are available to every PL/php
 * 		function in this session.  The table has columns (modname, modseq,
 * 		modsrc); rows are loaded ordered by (modname, modseq).
 *
 * Must be called with an active SPI connection.
 */
static void
plphp_load_modules(void)
{
	static long	module_counter = 0;
	uint64		i;
	int			ret;

	/* Nothing to do unless a visible plphp_modules table exists */
	ret = SPI_execute("SELECT 1 FROM pg_class "
					  "WHERE relname = 'plphp_modules' "
					  "AND relkind IN ('r', 'p') "
					  "AND pg_table_is_visible(oid)", true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed == 0)
		return;

	ret = SPI_execute("SELECT modsrc FROM plphp_modules "
					  "ORDER BY modname, modseq", true, 0);
	if (ret != SPI_OK_SELECT)
		return;

	for (i = 0; i < SPI_processed; i++)
	{
		char   *modsrc = SPI_getvalue(SPI_tuptable->vals[i],
									  SPI_tuptable->tupdesc, 1);
		char	fname[64];
		char   *src;
		char   *exmsg;
		zval	fn,
				rv;

		if (modsrc == NULL)
			continue;

		/*
		 * Wrap the module source in a uniquely-named function and run it: any
		 * function or class declarations it contains are hoisted to global
		 * scope when the wrapper executes.
		 */
		snprintf(fname, sizeof(fname), "plphp_module_%ld", ++module_counter);
		src = palloc(strlen(modsrc) + strlen(fname) +
					 strlen("function (){}") + 1);
		sprintf(src, "function %s(){%s}", fname, modsrc);

		zend_hash_str_del(CG(function_table), fname, strlen(fname));
		if (zend_eval_string(src, NULL, "plphp module") == FAILURE)
		{
			exmsg = plphp_pop_exception_message();
			if (exmsg != NULL)
				elog(ERROR, "plphp: failed to load module: %s", exmsg);
			else
				elog(ERROR, "plphp: failed to load module");
		}
		pfree(src);

		ZVAL_STRING(&fn, fname);
		ZVAL_UNDEF(&rv);
		if (call_user_function(CG(function_table), NULL, &fn, &rv,
							   0, NULL) == FAILURE)
			elog(ERROR, "plphp: failed to run module");

		exmsg = plphp_pop_exception_message();
		zval_ptr_dtor(&fn);
		zval_ptr_dtor(&rv);
		zend_hash_str_del(CG(function_table), fname, strlen(fname));
		pfree(modsrc);

		if (exmsg != NULL)
			elog(ERROR, "plphp: error while loading module: %s", exmsg);
	}
}

/*
 * plphp_session_init
 * 		Run once per session on the first PL/php use (with SPI connected):
 * 		load modules from plphp_modules and invoke plphp.start_proc.
 */
static void
plphp_session_init(void)
{
	if (plphp_session_inited)
		return;
	plphp_session_inited = true;

	plphp_load_modules();

	if (plphp_start_proc != NULL && plphp_start_proc[0] != '\0')
	{
		StringInfoData	buf;
		int				ret;

		initStringInfo(&buf);
		appendStringInfo(&buf, "SELECT %s()", plphp_start_proc);
		ret = SPI_execute(buf.data, false, 0);
		if (ret < 0)
			elog(ERROR, "plphp: start_proc \"%s\" failed: %s",
				 plphp_start_proc, SPI_result_code_string(ret));
		pfree(buf.data);
	}
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
	bool		nonatomic = false;

	/* Initialize interpreter */
	plphp_init_all();

	/*
	 * When invoked as a procedure via CALL in a non-atomic context, the code
	 * is allowed to commit/rollback (spi_commit/spi_rollback), which requires a
	 * non-atomic SPI connection.
	 */
	if (fcinfo->context && IsA(fcinfo->context, CallContext))
		nonatomic = !((CallContext *) fcinfo->context)->atomic;

	PG_TRY();
	{
		/* Connect to SPI manager */
		if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to SPI manager")));

		zend_try
		{
			plphp_proc_desc *desc;

			/* Clean up SRF state */
			current_fcinfo = NULL;

			/* On the first call in this session, load modules / start_proc */
			plphp_session_init();

			/* Redirect to the appropiate handler */
			if (CALLED_AS_TRIGGER(fcinfo))
			{
				desc = plphp_compile_function(fcinfo->flinfo->fn_oid,
											  true, false);

				retval = plphp_trigger_handler(fcinfo, desc);
			}
			else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
			{
				desc = plphp_compile_function(fcinfo->flinfo->fn_oid,
											  false, true);

				retval = plphp_event_trigger_handler(fcinfo, desc);
			}
			else
			{
				desc = plphp_compile_function(fcinfo->flinfo->fn_oid,
											  false, false);

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
 * plphp_inline_handler
 *
 * 		Execute an anonymous PL/php code block (a DO statement).
 */
Datum
plphp_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock =
		(InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));
	static long	inline_counter = 0;
	char		funcname[64];
	char	   *complete_source;
	zval		funcname_zv;
	zval		retval;

	/* Initialize interpreter */
	plphp_init_all();

	PG_TRY();
	{
		if (SPI_connect() != SPI_OK_CONNECT)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not connect to SPI manager")));

		zend_try
		{
			char   *exmsg;

			/* Clean up SRF state */
			current_fcinfo = NULL;

			/* Wrap the block body in a uniquely-named parameterless function */
			snprintf(funcname, sizeof(funcname), "plphp_inline_%ld",
					 ++inline_counter);

			complete_source = (char *) palloc(strlen(codeblock->source_text) +
											  strlen(funcname) +
											  strlen("function (){}") + 1);
			sprintf(complete_source, "function %s(){%s}", funcname,
					codeblock->source_text);

			zend_hash_str_del(CG(function_table), funcname, strlen(funcname));

			if (zend_eval_string(complete_source, NULL,
								 "plphp inline code block") == FAILURE)
			{
				exmsg = plphp_pop_exception_message();
				if (exmsg != NULL)
					elog(ERROR, "unable to compile inline code block: %s", exmsg);
				else
					elog(ERROR, "unable to compile inline code block");
			}
			pfree(complete_source);

			ZVAL_STRING(&funcname_zv, funcname);
			ZVAL_UNDEF(&retval);

			if (call_user_function(CG(function_table), NULL, &funcname_zv,
								   &retval, 0, NULL) == FAILURE)
				elog(ERROR, "could not execute inline code block");

			exmsg = plphp_pop_exception_message();

			zval_ptr_dtor(&funcname_zv);
			zval_ptr_dtor(&retval);
			zend_hash_str_del(CG(function_table), funcname, strlen(funcname));

			if (exmsg != NULL)
				elog(ERROR, "%s", exmsg);
		}
		zend_catch
		{
			if (error_msg)
			{
				char	str[1024];

				strlcpy(str, error_msg, sizeof(str));
				pfree(error_msg);
				error_msg = NULL;
				elog(ERROR, "%s", str);
			}
			else
				elog(ERROR, "fatal error");
		}
		zend_end_try();

		if (SPI_finish() != SPI_OK_FINISH)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
					 errmsg("could not disconnect from SPI manager")));
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_VOID();
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

	/* Initialize interpreter */
	plphp_init_all();

	PG_TRY();
	{
		bool			isnull;
		/* Grab the pg_proc tuple */
		procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
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
		prosrc = TextDatumGetCString(prosrcdatum);

		/* Get the function name, for the error message */
		strlcpy(funcname, NameStr(procForm->proname), NAMEDATALEN);

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
			zend_hash_str_del(CG(function_table), tmpname, strlen(tmpname));

			/*
			 * Let the user see the fireworks.  If the function doesn't validate,
			 * the ERROR will be raised and the function will not be created.
			 */
			if (zend_eval_string(tmpsrc, NULL,
								 "plphp function temp source") == FAILURE)
			{
				/*
				 * In PHP 8 a parse failure is reported as a pending exception
				 * (a ParseError); pop it both to report the detail and to keep
				 * it from leaking into the next call in this session.
				 */
				char   *exmsg = plphp_pop_exception_message();

				if (exmsg != NULL)
					elog(ERROR, "function \"%s\" does not validate: %s",
						 funcname, exmsg);
				else
					elog(ERROR, "function \"%s\" does not validate", funcname);
			}

			pfree(tmpsrc);
			tmpsrc = NULL;

			/* Delete the newly-created function from the PHP function table. */
			zend_hash_str_del(CG(function_table), tmpname, strlen(tmpname));
		}
		zend_catch
		{
			if (tmpsrc != NULL)
				pfree(tmpsrc);

			if (error_msg)
			{
				char	str[1024];

				strlcpy(str, error_msg, sizeof(str));
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

	retval = (zval *) emalloc(sizeof(zval));
	array_init(retval);

	tdata = (TriggerData *) fcinfo->context;
	tupdesc = tdata->tg_relation->rd_att;

	/* The basic variables */
	add_assoc_string(retval, "name", tdata->tg_trigger->tgname);
    add_assoc_long(retval, "relid", tdata->tg_relation->rd_id);
	add_assoc_string(retval, "relname", SPI_getrelname(tdata->tg_relation));
	add_assoc_string(retval, "schemaname", SPI_getnspname(tdata->tg_relation));

	/* EVENT */
	if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
		add_assoc_string(retval, "event", "INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
		add_assoc_string(retval, "event", "DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		add_assoc_string(retval, "event", "UPDATE");
	else
		elog(ERROR, "unknown firing event for trigger function");

	/* NEW and OLD as appropiate */
	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
	{
		if (TRIGGER_FIRED_BY_INSERT(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			add_assoc_zval(retval, "new", hashref);
			efree(hashref);
		}
		else if (TRIGGER_FIRED_BY_DELETE(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			add_assoc_zval(retval, "old", hashref);
			efree(hashref);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(tdata->tg_event))
		{
			zval	   *hashref;

			hashref = plphp_build_tuple_argument(tdata->tg_newtuple, tupdesc);
			add_assoc_zval(retval, "new", hashref);
			efree(hashref);

			hashref = plphp_build_tuple_argument(tdata->tg_trigtuple, tupdesc);
			add_assoc_zval(retval, "old", hashref);
			efree(hashref);
		}
		else
			elog(ERROR, "unknown firing event for trigger function");
	}

	/* ARGC and ARGS */
	add_assoc_long(retval, "argc", tdata->tg_trigger->tgnargs);

	if (tdata->tg_trigger->tgnargs > 0)
	{
		zval	   *hashref;

		hashref = (zval *) emalloc(sizeof(zval));
		array_init(hashref);

		for (i = 0; i < tdata->tg_trigger->tgnargs; i++)
			add_index_string(hashref, i, tdata->tg_trigger->tgargs[i]);

		add_assoc_zval(retval, "args", hashref);
		efree(hashref);
	}

	/* WHEN */
	if (TRIGGER_FIRED_BEFORE(tdata->tg_event))
		add_assoc_string(retval, "when", "BEFORE");
	else if (TRIGGER_FIRED_AFTER(tdata->tg_event))
		add_assoc_string(retval, "when", "AFTER");
	else
		elog(ERROR, "unknown firing time for trigger function");

	/* LEVEL */
	if (TRIGGER_FIRED_FOR_ROW(tdata->tg_event))
		add_assoc_string(retval, "level", "ROW");
	else if (TRIGGER_FIRED_FOR_STATEMENT(tdata->tg_event))
		add_assoc_string(retval, "level", "STATEMENT");
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

	if (Z_TYPE_P(zTrigData) != IS_ARRAY)
		elog(ERROR, "$_TD is not an array");

	/*
	 * In a BEFORE trigger, compute the return value.  In an AFTER trigger
	 * it'll be ignored, so don't bother.
	 */
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
	{
		switch (Z_TYPE_P(phpret))
		{
			case IS_STRING:
				srv = Z_STRVAL_P(phpret);
				if (strcasecmp(srv, "SKIP") == 0)
				{
					/* do nothing */
					break;
				}
				else if (strcasecmp(srv, "MODIFY") == 0)
				{
					if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
						TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
						retval = PointerGetDatum(plphp_modify_tuple(zTrigData,
																	trigdata));
					else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("on delete trigger can not modify the the return tuple")));
					else
						elog(ERROR, "unknown event in trigger function");
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("expected trigger function to return NULL, 'SKIP' or 'MODIFY'")));
				break;
			case IS_NULL:
				if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) ||
					TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
					retval = (Datum) trigdata->tg_trigtuple;
				else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
					retval = (Datum) trigdata->tg_newtuple;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("expected trigger function to return NULL, 'SKIP' or 'MODIFY'")));
				break;
		}
	}

	REPORT_PHP_MEMUSAGE("freeing some variables");

	zval_ptr_dtor(zTrigData);
	efree(zTrigData);
	zval_ptr_dtor(phpret);
	efree(phpret);

	REPORT_PHP_MEMUSAGE("trigger call done");

	return retval;
}

/*
 * plphp_event_trigger_handler
 * 		Handler for event trigger function calls.  Builds $_TD with the
 * 		firing event and command tag, and calls the user function.
 */
static Datum
plphp_event_trigger_handler(FunctionCallInfo fcinfo, plphp_proc_desc *desc)
{
	EventTriggerData *tdata = (EventTriggerData *) fcinfo->context;
	zval	   *td;
	zval	   *phpret;
	zval		funcname;
	zval		param;
	char	   *exmsg;
	char		call[64];

	/* Build $_TD for the event trigger */
	td = (zval *) emalloc(sizeof(zval));
	array_init(td);
	add_assoc_string(td, "event", tdata->event);
#if PG_VERSION_NUM >= 130000
	/* PG 13 turned the command tag into a CommandTag enum */
	add_assoc_string(td, "tag", GetCommandTagName(tdata->tag));
#else
	add_assoc_string(td, "tag", tdata->tag);
#endif

	/* Call plphp_proc_NNN_evttrigger($_TD) */
	snprintf(call, sizeof(call), "plphp_proc_%u_evttrigger",
			 fcinfo->flinfo->fn_oid);
	ZVAL_STRING(&funcname, call);
	ZVAL_COPY_VALUE(&param, td);
	efree(td);

	phpret = (zval *) emalloc(sizeof(zval));
	ZVAL_UNDEF(phpret);

	if (call_user_function(CG(function_table), NULL, &funcname, phpret,
						   1, &param) == FAILURE)
		elog(ERROR, "could not call event trigger function %s", desc->proname);

	exmsg = plphp_pop_exception_message();

	zval_ptr_dtor(&funcname);
	zval_ptr_dtor(&param);
	zval_ptr_dtor(phpret);
	efree(phpret);

	if (exmsg != NULL)
		elog(ERROR, "%s", exmsg);

	/* Disconnect from SPI; the return value of an event trigger is ignored */
	if (SPI_finish() != SPI_OK_FINISH)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_DOES_NOT_EXIST),
				 errmsg("could not disconnect from SPI manager")));

	return (Datum) 0;
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
	if (!phpret)
		elog(ERROR, "error during execution of function %s", desc->proname);

	REPORT_PHP_MEMUSAGE("function invoked");

	/* Basic datatype checks */
	if ((desc->ret_type & PL_ARRAY) && Z_TYPE_P(phpret) != IS_ARRAY)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("function declared to return array must return an array")));
	if ((desc->ret_type & PL_TUPLE) && Z_TYPE_P(phpret) != IS_ARRAY)
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

		retTypeTup = SearchSysCache1(TYPEOID,
									 ObjectIdGetDatum(get_fn_expr_rettype(fcinfo->flinfo)));
		retTypeStruct = (Form_pg_type) GETSTRUCT(retTypeTup);
		perm_fmgr_info(retTypeStruct->typinput, &(desc->result_in_func));
		desc->result_typioparam = retTypeStruct->typelem;
		ReleaseSysCache(retTypeTup);
	}

	if (phpret)
	{
		switch (Z_TYPE_P(phpret))
		{
			case IS_NULL:
				fcinfo->isnull = true;
				break;
			case IS_TRUE:
			case IS_FALSE:
			case IS_DOUBLE:
			case IS_LONG:
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
					ReleaseTupleDesc(td);
				}
				else
					/* FIXME -- should return the thing as a string? */
					elog(ERROR, "this plphp function cannot return arrays");
				break;
			default:
				elog(WARNING,
					 "plphp functions cannot return type %i",
					 Z_TYPE_P(phpret));
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
		retval = InputFunctionCall(&desc->result_in_func,
								   retvalbuffer,
								   desc->result_typioparam,
								   -1);
		pfree(retvalbuffer);
	}

	/* Release the value returned by the PHP function */
	zval_ptr_dtor(phpret);
	efree(phpret);

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
										   ALLOCSET_DEFAULT_SIZES);

	/* Tuple descriptor and AttInMetadata for return_next */
	current_tupledesc = CreateTupleDescCopy(tupdesc);
	current_attinmeta = TupleDescGetAttInMetadata(current_tupledesc);

	/*
	 * Call the PHP function.  The user code must call return_next, which will
	 * create and populate the tuplestore appropiately.
	 */
	phpret = plphp_call_php_func(desc, fcinfo);

	/* We don't use the return value */
	zval_ptr_dtor(phpret);
	efree(phpret);

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
plphp_compile_function(Oid fnoid, bool is_trigger, bool is_event_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc procStruct;
	char		internal_proname[64];
	plphp_proc_desc *prodesc = NULL;
	int			i;
	char	   *pointer = NULL;

	/*
	 * We'll need the pg_proc tuple in any case... 
	 */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fnoid));
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fnoid);
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * Build our internal procedure name from the function's Oid
	 */
	if (is_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u_trigger", fnoid);
	else if (is_event_trigger)
		snprintf(internal_proname, sizeof(internal_proname),
				 "plphp_proc_%u_evttrigger", fnoid);
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
		bool uptodate;
		sscanf(pointer, "%p", &prodesc);

		uptodate =
			(prodesc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			 prodesc->fn_cmin == HeapTupleHeaderGetRawCommandId(procTup->t_data));

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
		Form_pg_language langStruct;
		Datum		prosrcdatum;
		bool		isnull;
		char	   *proc_source;
		char	   *complete_proc_source;
		char	   *pointer = NULL;
		char	   *aliases = NULL;
		char	   *out_aliases = NULL;
		char	   *out_return_str = NULL;
		int16	typlen;
		bool	typbyval;
		char	typalign,
				typtype,
				typdelim;
		Oid		typioparam,
				typinput,
				typoutput;
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
		prodesc->fn_cmin = HeapTupleHeaderGetRawCommandId(procTup->t_data);

		/*
		 * Look up the pg_language tuple by Oid
		 */
		langTup = SearchSysCache1(LANGOID,
								  ObjectIdGetDatum(procStruct->prolang));
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
		if (!is_trigger && !is_event_trigger)
		{
			char  **argnames;
			char   *argmodes;
			Oid    *argtypes;
			int32	alias_str_end,
					out_str_end;

			typtype = get_typtype(procStruct->prorettype);
			get_type_io_data(procStruct->prorettype,
							 IOFunc_input,
							 &typlen,
							 &typbyval,
							 &typalign,
							 &typdelim,
							 &typioparam,
							 &typinput);

			/*
			 * Disallow pseudotype result, except:
			 * VOID, RECORD, ANYELEMENT or ANYARRAY
			 */
			if (typtype == TYPTYPE_PSEUDO)
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

			if (typtype == TYPTYPE_COMPOSITE ||
				procStruct->prorettype == RECORDOID)
			{
				prodesc->ret_type |= PL_TUPLE;
			}

			if (procStruct->prorettype == ANYARRAYOID)
				prodesc->ret_type |= PL_ARRAY;
			else
			{
				/* function returns a normal (declared) array */
				if (typlen == -1 && get_element_type(procStruct->prorettype))
					prodesc->ret_type |= PL_ARRAY;
			}

			perm_fmgr_info(typinput, &(prodesc->result_in_func));
			prodesc->result_typioparam = typioparam;

			/* Deal with named arguments, OUT, IN/OUT and TABLE arguments */

			prodesc->n_total_args = get_func_arg_info(procTup, &argtypes, 
											  		  &argnames, &argmodes);
			prodesc->n_out_args = 0;
			prodesc->n_mixed_args = 0;
			
			prodesc->args_out_tupdesc = NULL;
			out_return_str = NULL;
			alias_str_end = out_str_end = 0;

			/* Count the number of OUT arguments. Need to do this out of the
			 * main loop, to correctly determine the object to return for OUT args
		     */
			if (argmodes)
				for (i = 0; i < prodesc->n_total_args; i++)
				{
					switch(argmodes[i])
					{
						case PROARGMODE_OUT: 
							prodesc->n_out_args++;
							break;
						case PROARGMODE_INOUT: 
							prodesc->n_mixed_args++;
							break;
						case PROARGMODE_IN:
							break;
						case PROARGMODE_TABLE:
							break;
						case PROARGMODE_VARIADIC:
							elog(ERROR, "VARIADIC arguments are not supported");
						default:
							elog(ERROR, "Unsupported type %c for argument no %d",
								 argmodes[i], i);
					}					
					prodesc->arg_argmode[i] = argmodes[i];
				}
			else
				MemSet(prodesc->arg_argmode, PROARGMODE_IN,
				 	   prodesc->n_total_args);

			/* Allocate memory for argument names unless all of them are OUT*/
			if (argnames && prodesc->n_total_args > 0)
				aliases = palloc((NAMEDATALEN + 32) * prodesc->n_total_args);
			
			/* Main argument processing loop. */
			for (i = 0; i < prodesc->n_total_args; i++)
			{
				prodesc->arg_typtype[i] = get_typtype(argtypes[i]);
				if (prodesc->arg_typtype[i] != TYPTYPE_COMPOSITE)
				{							
					get_type_io_data(argtypes[i],
									 IOFunc_output,
									 &typlen,
									 &typbyval,
									 &typalign,
									 &typdelim,
									 &typioparam,
									 &typoutput);
					perm_fmgr_info(typoutput, &(prodesc->arg_out_func[i]));
					prodesc->arg_typioparam[i] = typioparam;
				}
				if (aliases && argnames[i][0] != '\0')
				{
					if (!is_valid_php_identifier(argnames[i]))
						elog(ERROR, "\"%s\" can not be used as a PHP variable name",
							 argnames[i]);
					/* Deal with argument name */
					alias_str_end += snprintf(aliases + alias_str_end,
										 	  NAMEDATALEN + 32,
								   		 	  " $%s = &$args[%d];", 
											  argnames[i], i);
				}
				if ((prodesc->arg_argmode[i] == PROARGMODE_OUT ||
					 prodesc->arg_argmode[i] == PROARGMODE_INOUT) && !prodesc->retset)
				{
					/* Initialiazation for OUT arguments aliases */
					if (!out_return_str)
					{
						/* Generate return statment for a single OUT argument */
						out_return_str = palloc(NAMEDATALEN + 32);
						if (prodesc->n_out_args + prodesc->n_mixed_args == 1)
							snprintf(out_return_str, NAMEDATALEN + 32,
									 "return $args[%d];", i);
						else
						{
							/* PL/PHP deals with multiple OUT arguments by
							 * internally creating an array of references to them.
							 * E.g. out_fn(a out integer, b out integer )
							 * translates into:
							 * $_plphp_ret_out_fn_1234=array(a => $&a,b => $&b);
							 */
							char plphp_ret_array_name[NAMEDATALEN + 16];

							int array_namelen = snprintf(plphp_ret_array_name,
							 						 	 NAMEDATALEN + 16,
									 				 	 "_plphp_ret_%s",
													 	 internal_proname);

							snprintf(out_return_str, array_namelen + 16,
									"return $%s;", plphp_ret_array_name);
									
							/* 2 NAMEDATALEN for argument names, additional
							 * 16 bytes per each argument for assignment string,
							 * additional 16 bytes for the 'array' prefix string.
							 */		
							out_aliases = palloc(array_namelen +
												 (prodesc->n_out_args + 
												  prodesc->n_mixed_args) *
												 (2*NAMEDATALEN + 16) + 16);
												
							out_str_end = snprintf(out_aliases,
							 					   array_namelen +
												   (2 * NAMEDATALEN + 16) + 16,
												   "$%s = array(&$args[%d]", 
												   plphp_ret_array_name, i);
												   
						}
					} 
					else if (out_aliases)
					{
					   /* Add new elements to the array of aliases for OUT args */
						Assert(prodesc->n_out_args + prodesc->n_mixed_args > 1);
						out_str_end += snprintf(out_aliases+out_str_end,
												2 * NAMEDATALEN + 16,
												",&$args[%d]", i);
					}
				}
			}
			if (aliases)
				strcat(aliases, " ");
			if (out_aliases)
				strcat(out_aliases, ")");
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

		proc_source = TextDatumGetCString(prosrcdatum);

		/* Create the procedure in the interpreter */
		complete_proc_source =
			(char *) palloc(strlen(proc_source) +
							strlen(internal_proname) +
							(aliases ? strlen(aliases) : 0) + 
							(out_aliases ? strlen(out_aliases) : 0) +
							strlen("function  ($args, $argc){ } ") + 32 +
							(out_return_str ? strlen(out_return_str) : 0));

		/* XXX Is this usage of sprintf safe? */
		if (is_trigger)
			/* $_TD is passed by reference so the function can modify NEW */
			sprintf(complete_proc_source, "function %s(&$_TD){%s}",
					internal_proname, proc_source);
		else if (is_event_trigger)
			sprintf(complete_proc_source, "function %s($_TD){%s}",
					internal_proname, proc_source);
		else
			sprintf(complete_proc_source, 
					"function %s($args, $argc){%s %s;%s; %s}",
					internal_proname, 
					aliases ? aliases : "",
					out_aliases ? out_aliases : "",
					proc_source, 
					out_return_str? out_return_str : "");
					
		elog(LOG, "complete_proc_source = %s",
				 	 complete_proc_source);
				
		zend_hash_str_del(CG(function_table), prodesc->proname,
						  strlen(prodesc->proname));

		pointer = (char *) palloc(64);
		sprintf(pointer, "%p", (void *) prodesc);
		add_assoc_string(plphp_proc_array, internal_proname, (char *) pointer);

		if (zend_eval_string(complete_proc_source, NULL,
							 "plphp function source") == FAILURE)
		{
			char   *exmsg = plphp_pop_exception_message();

			/* the next compilation will blow it up */
			prodesc->fn_xmin = InvalidTransactionId;
			if (exmsg != NULL)
				elog(ERROR, "unable to compile function \"%s\": %s",
					 prodesc->proname, exmsg);
			else
				elog(ERROR, "unable to compile function \"%s\"",
					 prodesc->proname);
		}

		if (aliases)
			pfree(aliases);
		if (out_aliases)
			pfree(out_aliases);
		if (out_return_str)
			pfree(out_return_str);
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
	int			i,j;

	retval = (zval *) emalloc(sizeof(zval));
	array_init(retval);

	/*
	 * The first var iterates over every argument, the second one - over the
	 * IN or INOUT ones only
	 */
	for (i = 0, j = 0; i < desc->n_total_args;
		 (j = IS_ARGMODE_OUT(desc->arg_argmode[i]) ? j : j + 1), i++)
	{
		/* Assign NULLs to OUT or TABLE arguments initially */
		if (IS_ARGMODE_OUT(desc->arg_argmode[i]))
		{
			add_next_index_null(retval);
			continue;
		}

		if (desc->arg_typtype[i] == TYPTYPE_PSEUDO)
		{
			HeapTuple	typeTup;
			Form_pg_type typeStruct;

			typeTup = SearchSysCache1(TYPEOID,
									  ObjectIdGetDatum(get_fn_expr_argtype
													   (fcinfo->flinfo, j)));
			typeStruct = (Form_pg_type) GETSTRUCT(typeTup);
			perm_fmgr_info(typeStruct->typoutput,
						   &(desc->arg_out_func[i]));
			desc->arg_typioparam[i] = typeStruct->typelem;
			ReleaseSysCache(typeTup);
		}

		if (desc->arg_typtype[i] == TYPTYPE_COMPOSITE)
		{
			if (PLPHP_ARG_ISNULL(fcinfo, j))
				add_next_index_null(retval);
			else
			{
				HeapTupleHeader	td;
				Oid				tupType;
				int32			tupTypmod;
				TupleDesc		tupdesc;
				HeapTupleData	tmptup;
				zval		   *hashref;

				td = DatumGetHeapTupleHeader(PLPHP_ARG_VALUE(fcinfo, j));

				/* Build a temporary HeapTuple control structure */
				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;

				/* Extract rowtype info and find a tupdesc */
				tupType = HeapTupleHeaderGetTypeId(tmptup.t_data);
				tupTypmod = HeapTupleHeaderGetTypMod(tmptup.t_data);
				tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

				/* Build the PHP hash */
				hashref = plphp_build_tuple_argument(&tmptup, tupdesc);
				add_next_index_zval(retval, hashref);
				efree(hashref);
				/* Finally release the acquired tupledesc */
				ReleaseTupleDesc(tupdesc);
			}
		}
		else
		{
			if (PLPHP_ARG_ISNULL(fcinfo, j))
				add_next_index_null(retval);
			else
			{
				char	   *tmp;

				/*
				 * TODO room for improvement here: instead of going through the
				 * output function, figure out if we can just use the native
				 * representation to pass to PHP.
				 */
				tmp = OutputFunctionCall(&(desc->arg_out_func[i]),
										 PLPHP_ARG_VALUE(fcinfo, j));
				/*
				 * FIXME -- this is bogus.  Not every value starting with { is
				 * an array.  Figure out a better method for detecting arrays.
				 */
				if (tmp[0] == '{')
				{
					zval	   *hashref;

					hashref = plphp_convert_from_pg_array(tmp);
					add_next_index_zval(retval, hashref);
					efree(hashref);
				}
				else
					add_next_index_string(retval, tmp);

				pfree(tmp);
			}
		}
	}

	return retval;
}

/*
 * plphp_pop_exception_message
 * 		If a PHP exception is pending, return its message (palloc'd in the
 * 		current context) and clear the exception; otherwise return NULL.
 *
 * In PHP 8 many conditions that used to be fatal errors (calling an undefined
 * function, type errors, ...) are thrown as exceptions instead of routed
 * through zend_error_cb, so we must check for them explicitly after invoking
 * user code and translate them into Postgres errors.
 */
static char *
plphp_pop_exception_message(void)
{
	zend_object *ex = EG(exception);
	zval		 rv;
	zval		*zmsg;
	char		*msg;

	if (ex == NULL)
		return NULL;

	zmsg = zend_read_property(ex->ce, ex, "message", sizeof("message") - 1,
							  1, &rv);
	if (zmsg != NULL && Z_TYPE_P(zmsg) == IS_STRING && Z_STRLEN_P(zmsg) > 0)
		msg = pstrdup(Z_STRVAL_P(zmsg));
	else
		msg = pstrdup("uncaught PHP exception");

	zend_clear_exception();
	return msg;
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
	zval		funcname;
	zval		params[2];
	char		call[64];

	REPORT_PHP_MEMUSAGE("going to build function args");

	/*
	 * Build the function arguments.  The generated PHP function takes two
	 * parameters, $args and $argc.
	 */
	args = plphp_func_build_args(desc, fcinfo);
	ZVAL_COPY_VALUE(&params[0], args);	/* move the array into params[0] */
	efree(args);
	ZVAL_LONG(&params[1], desc->n_total_args);

	REPORT_PHP_MEMUSAGE("args built. Now the rest ...");

	/* Build the internal function name */
	sprintf(call, "plphp_proc_%u", fcinfo->flinfo->fn_oid);
	ZVAL_STRING(&funcname, call);

	/* Result zval; ownership passes to the caller */
	retval = (zval *) emalloc(sizeof(zval));
	ZVAL_UNDEF(retval);

	REPORT_PHP_MEMUSAGE("going to call the function");

	if (call_user_function(CG(function_table), NULL, &funcname, retval,
						   2, params) == FAILURE)
		elog(ERROR, "could not call function \"%s\"", call);

	REPORT_PHP_MEMUSAGE("going to free some vars");

	/* Propagate any uncaught PHP exception as a Postgres error */
	{
		char   *exmsg = plphp_pop_exception_message();

		if (exmsg != NULL)
		{
			zval_ptr_dtor(&funcname);
			zval_ptr_dtor(&params[0]);
			zval_ptr_dtor(retval);
			efree(retval);
			elog(ERROR, "%s", exmsg);
		}
	}

	zval_ptr_dtor(&funcname);
	zval_ptr_dtor(&params[0]);	/* the $args array */
	/* params[1] holds a long, nothing to free */

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
	zval		funcname;
	zval		params[1];
	char		call[64];

	/*
	 * The generated trigger function takes $_TD by reference, so it can
	 * modify NEW.  Wrap the $_TD array in a reference and pass that; after
	 * the call we copy the (possibly updated) array back into *trigdata so
	 * the caller sees the modifications.
	 */
	ZVAL_NEW_REF(&params[0], trigdata);

	/* Build the internal function name */
	sprintf(call, "plphp_proc_%u_trigger", fcinfo->flinfo->fn_oid);
	ZVAL_STRING(&funcname, call);

	retval = (zval *) emalloc(sizeof(zval));
	ZVAL_UNDEF(retval);

	if (call_user_function(CG(function_table), NULL, &funcname, retval,
						   1, params) == FAILURE)
		elog(ERROR, "could not call function \"%s\"", call);

	/* Propagate any uncaught PHP exception as a Postgres error */
	{
		char   *exmsg = plphp_pop_exception_message();

		if (exmsg != NULL)
		{
			zval_ptr_dtor(&params[0]);
			zval_ptr_dtor(&funcname);
			zval_ptr_dtor(retval);
			efree(retval);
			elog(ERROR, "%s", exmsg);
		}
	}

	/* Copy the possibly-updated $_TD array back into the caller's zval */
	ZVAL_COPY_VALUE(trigdata, Z_REFVAL(params[0]));
	Z_TRY_ADDREF_P(trigdata);
	zval_ptr_dtor(&params[0]);
	zval_ptr_dtor(&funcname);

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
plphp_error_cb(int type, zend_string *error_filename,
			   const uint32_t error_lineno, zend_string *message)
{
	const char *str = ZSTR_VAL(message);
	const char *trace;
	int		elevel;

	/*
	 * PHP formats an uncaught exception as "...message... in <file>:<line>\n
	 * Stack trace:\n#0 ... plphp_proc_<oid>() ...".  The stack trace carries
	 * the internal, OID-derived function name, which is both noisy and
	 * non-deterministic, so drop everything from "Stack trace:" onwards.
	 */
	trace = strstr(str, "\nStack trace:");
	if (trace != NULL)
		str = pnstrdup(str, trace - str);

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
		case E_DEPRECATED:
		case E_USER_DEPRECATED:
			/*
			 * Deprecations (e.g. PHP 8.2's "${var}" string interpolation) must
			 * not be fatal, so map them to NOTICE rather than falling through
			 * to the ERROR default below.
			 */
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
		if (error_lineno != 0)
		{
			char	msgline[1024];
			snprintf(msgline, sizeof(msgline), "%s at line %u", str, error_lineno);
			error_msg = pstrdup(msgline);
		}
		else
			error_msg = pstrdup(str);

		zend_bailout();
	}

	ereport(elevel,
			(errmsg("plphp: %s", str)));
}

/* Check if the name can be a valid PHP variable name */
static bool 
is_valid_php_identifier(char *name)
{
	int 	len,
			i;
	
	Assert(name);

	len = strlen(name);

	/* Should start from the letter */
	if (!isalpha(name[0]))
		return false;
	for (i = 1; i < len; i++)
	{
		/* Only letters, digits and underscores are allowed */
		if (!isalpha(name[i]) && !isdigit(name[i]) && name[i] != '_')
			return false;
	}
	return true;
}

/*
 * vim:ts=4:sw=4:cino=(0
 */
