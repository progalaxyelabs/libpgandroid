/*
 * pgandroid/pgandroid_stubs.h  (v2)
 *
 * Stub implementations needed when linking PostgreSQL backend + frontend
 * tools into a single shared library (libpgandroid.so).
 *
 * Included into pgandroid_main.c (backend TU).
 *
 * v2 changes: Removed pg_proc_exit (v2 uses real proc_exit with #ifdef
 * guards at call sites). Removed popen/pclose intercepts (handled in
 * pgandroid_initdb.c). Removed duplicate stubs already in pgandroid_io.c.
 */

#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * PostgresMain stub — only used in multi-process mode (postmaster → fork).
 * Not needed in pgandroid single-process mode.
 * ----------------------------------------------------------------------- */
#ifndef PGANDROID_HAVE_POSTGRESMAIN
void
PostgresMain(const char *dbname, const char *username)
{
    abort();
}
#endif

/* -----------------------------------------------------------------------
 * simple_prompt — returns empty string (no interactive prompting in JNI)
 * ----------------------------------------------------------------------- */
char *
simple_prompt(const char *prompt, bool echo)
{
    (void) prompt;
    (void) echo;
    char *r = strdup("");
    if (!r) abort();
    return r;
}

/* -----------------------------------------------------------------------
 * ProcessStartupPacket stub (multi-process path, not used in single mode)
 * ----------------------------------------------------------------------- */
int
ProcessStartupPacket(struct Port *port, bool ssl_done, bool gss_done)
{
    (void) port; (void) ssl_done; (void) gss_done;
    return 0;  /* STATUS_OK */
}

