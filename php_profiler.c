/*
  +----------------------------------------------------------------------+
  | PHP Version 8                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2021 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <joe.watkins@live.co.uk>                         |
  | Updated for PHP 8: Mustapha.A https://github.com/stoufa06/        |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_streams.h"
#include "ext/standard/info.h"
#include "zend_extensions.h" // Required for zend_execute_ex, zend_execute_internal
#include "zend_string.h"   // Required for zend_string
#include "zend_ini.h"      // Required for INI macros

#include "php_profiler.h"

ZEND_DECLARE_MODULE_GLOBALS(profiler)

static zend_bool profiler_initialized = 0;

#ifndef _WIN32
static __inline__ ticks_t ticks(void) {
	unsigned x, y;
	asm volatile ("rdtsc" : "=a" (x), "=d" (y));
	return ((((ticks_t)x) | ((ticks_t)y) << 32));
}
#else
#   include <intrin.h>
static inline ticks_t ticks(void) {
	return __rdtsc();
}
#endif

/* PHP 8 function prototypes */
static void (*zend_execute_old)(zend_execute_data *execute_data);
static void (*zend_execute_internal_old)(zend_execute_data *execute_data, zend_fcall_info *fci, zval *return_value);
static void profiler_execute(zend_execute_data *execute_data);
static void profiler_execute_internal(zend_execute_data *execute_data, zend_fcall_info *fci, zval *return_value);

/* Argument Information */
ZEND_BEGIN_ARG_INFO_EX(arginfo_profiler_enable, 0, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_profiler_output, 0, 0, 1, 1)
	ZEND_ARG_INFO(0, filename)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_profiler_disable, 0, 0, 0, 0)
ZEND_END_ARG_INFO()

const zend_function_entry profiler_functions[] = {
	PHP_FE(profiler_enable, arginfo_profiler_enable)
	PHP_FE(profiler_output, arginfo_profiler_output)
	PHP_FE(profiler_disable, arginfo_profiler_disable)
	PHP_FE_END
};

zend_module_entry profiler_module_entry = {
	STANDARD_MODULE_HEADER,
	PROFILER_NAME,
	profiler_functions,
	PHP_MINIT(profiler),
	PHP_MSHUTDOWN(profiler),
	PHP_RINIT(profiler),
	PHP_RSHUTDOWN(profiler),
	PHP_MINFO(profiler),
	PROFILER_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PROFILER
ZEND_GET_MODULE(profiler)
#endif

PHP_INI_BEGIN()
PHP_INI_ENTRY("profiler.enabled", "0", PHP_INI_SYSTEM, NULL)
PHP_INI_ENTRY("profiler.memory", "1", PHP_INI_SYSTEM, NULL)
PHP_INI_ENTRY("profiler.output", "/tmp/profile.callgrind", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

static inline void profiler_globals_ctor(zend_profiler_globals *pg) {
	pg->enabled = 0;
	pg->memory = 1;
	pg->output = NULL;
	pg->reset = 0;
	pg->frame = &pg->frames[0];
	pg->limit = &pg->frames[PROFILER_MAX_FRAMES];
}

PHP_MINIT_FUNCTION(profiler)
{
	if (profiler_initialized) {
		return SUCCESS;
	}

	profiler_initialized = 1;

	/* PHP 8 specific initialization */
	zend_execute_old = zend_execute_ex;
	zend_execute_ex = profiler_execute;
	zend_execute_internal_old = zend_execute_internal;
	zend_execute_internal = profiler_execute_internal;

	REGISTER_INI_ENTRIES();

	ZEND_INIT_MODULE_GLOBALS(profiler, profiler_globals_ctor, NULL);

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(profiler)
{
	UNREGISTER_INI_ENTRIES();

	/* PHP 8 specific shutdown */
	zend_execute_ex = zend_execute_old;
	zend_execute_internal = zend_execute_internal_old;

	return SUCCESS;
}

PHP_RINIT_FUNCTION(profiler)
{
	PROF_G(enabled) = INI_BOOL("profiler.enabled");
	PROF_G(memory) = INI_BOOL("profiler.memory");
	PROF_G(output) = INI_STR("profiler.output");
	PROF_G(frame) = &PROF_G(frames)[0]; // Reset frame pointer for each request

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(profiler)
{
	if (PROF_G(output)) {
		php_stream *stream = php_stream_open_wrapper(PROF_G(output), "w", REPORT_ERRORS, NULL);
		if (stream) {
			profile_t *profile, *end;

			php_stream_printf(stream, "version: 1\n");
			php_stream_printf(stream, "creator: profiler\n");
			php_stream_printf(stream, "pid: %d\n", getpid());
			if (PROF_G(memory)) {
				php_stream_printf(stream, "events: memory cpu\n");
			} else {
				php_stream_printf(stream, "events: cpu\n");
			}

			profile = &PROF_G(frames)[0];
			end = PROF_G(frame);

			if (profile < end) {
				do {
					if (profile) {
						php_stream_printf(stream, "fl=%s\n", profile->location.file ? profile->location.file->val : "");
						if (profile->call.scope && profile->call.scope->len) {
							php_stream_printf(stream, "fn=%s::%s\n", profile->call.scope->val, profile->call.function ? profile->call.function->val : "");
						} else {
							php_stream_printf(stream, "fn=%s\n", profile->call.function ? profile->call.function->val : "");
						}

						if (PROF_G(memory)) {
							php_stream_printf(
								stream,
								"%ld %lld %lld\n",
								profile->location.line, profile->call.memory, profile->call.cpu
							);
						} else {
							php_stream_printf(
								stream,
								"%ld %lld\n",
								profile->location.line, profile->call.cpu
							);
						}
						php_stream_printf(stream, "\n");
					} else {
						break;
					}
				} while (++profile < end);
			}
			php_stream_close(stream);
		} else {
			php_error_docref(NULL, E_WARNING, "the profiler has failed to open %s for writing", PROF_G(output));
		}

		if (PROF_G(reset)) {
			zend_string_release(PROF_G(output));
			PROF_G(output) = NULL;
			PROF_G(reset) = 0;
		}
	}

	return SUCCESS;
}

PHP_MINFO_FUNCTION(profiler)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "profiler support", PROF_G(enabled) ? "enabled" : "disabled");
	php_info_print_table_row(2, "Profiler Enabled", PROF_G(enabled) ? "Yes" : "No");
	php_info_print_table_row(2, "Memory Tracking", PROF_G(memory) ? "Yes" : "No");
	php_info_print_table_row(2, "Output File", PROF_G(output) ? PROF_G(output)->val : "/tmp/profile.callgrind");
	php_info_print_table_end();
}

/* {{{ proto void profiler_enable()
   Enable the profiler */
PHP_FUNCTION(profiler_enable)
{
	if (!PROF_G(enabled)) {
		PROF_G(enabled) = 1;
	} else {
		php_error_docref(NULL, E_WARNING, "the profiler is already enabled");
	}
}
/* }}} */

/* {{{ proto void profiler_output(string filename)
   Set the output filename for the profiler data */
PHP_FUNCTION(profiler_output)
{
	zend_string *fpath;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_STR(fpath)
	ZEND_PARSE_PARAMETERS_END();

	if (PROF_G(reset) && PROF_G(output)) {
		zend_string_release(PROF_G(output));
	}

	PROF_G(output) = zend_string_copy(fpath);
	PROF_G(reset) = 1;
}
/* }}} */

/* {{{ proto void profiler_disable()
   Disable the profiler */
PHP_FUNCTION(profiler_disable)
{
	if (PROF_G(enabled)) {
		PROF_G(enabled) = 0;
	} else {
		php_error_docref(NULL, E_WARNING, "the profiler is already disabled");
	}
}
/* }}} */

static void profiler_record_call(zend_execute_data *execute_data)
{
	ulong line = zend_get_executed_lineno();
	profile_t *profile = PROF_G(frame)++;

	profile->location.file = zend_get_executed_filename();
	profile->location.line = line;
	profile->call.function = execute_data->func->common.function_name;
	profile->call.scope = execute_data->func->common.scope ? execute_data->func->common.scope->name : NULL;

	if (PROF_G(memory)) {
		profile->call.memory = zend_memory_usage(0);
	}

	profile->call.cpu = ticks();

	/* Store the current execute_data for the return hook */
	execute_data->prev_execute_data->profiler_frame = profile;
}

static void profiler_record_return(zend_execute_data *execute_data)
{
	zend_execute_data *prev = execute_data->prev_execute_data;
	if (prev && prev->profiler_frame) {
		profile_t *profile = prev->profiler_frame;
		profile->call.cpu = ticks() - profile->call.cpu;
		if (PROF_G(memory)) {
			profile->call.memory = zend_memory_usage(0) - profile->call.memory;
		}
		/* Clear the stored frame */
		prev->profiler_frame = NULL;
	}
}

static void profiler_execute(zend_execute_data *execute_data)
{
	if (PROF_G(enabled) && (PROF_G(frame) < PROF_G(limit)) && execute_data && execute_data->func) {
		profiler_record_call(execute_data);
		zend_execute_old(execute_data);
		profiler_record_return(execute_data);
	} else {
		zend_execute_old(execute_data);
	}
}

static void profiler_execute_internal(zend_execute_data *execute_data, zend_fcall_info *fci, zval *return_value)
{
	if (PROF_G(enabled) && (PROF_G(frame) < PROF_G(limit)) && execute_data && execute_data->func) {
		profiler_record_call(execute_data);
		zend_execute_internal_old(execute_data, fci, return_value);
		profiler_record_return(execute_data);
	} else {
		zend_execute_internal_old(execute_data, fci, return_value);
	}
}