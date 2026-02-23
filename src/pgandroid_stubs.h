/*
 * pgandroid/pgandroid_stubs.h
 *
 * Stub implementations needed when linking PostgreSQL backend + frontend
 * tools into a single shared library (libpgandroid.so).
 *
 * Mirrors pglite-wasm/pgl_stubs.h.
 *
 * Included into pgandroid_main.c (single-TU compilation).
 */

#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * PostgresMain stub
 * The real entry point is PostgresSingleUserMain().  PostgresMain is only
 * called in multi-process mode (postmaster → fork → PostgresMain).
 * ----------------------------------------------------------------------- */
#ifndef PGANDROID_HAVE_POSTGRESMAIN
void
PostgresMain(const char *dbname, const char *username)
{
    /* Should never be called in pgandroid single-process mode */
    abort();
}
#endif

/* -----------------------------------------------------------------------
 * startup_hacks — initialize spinlocks (called early in backend startup)
 * ----------------------------------------------------------------------- */
static void
startup_hacks(const char *progname)
{
    /* Initialize spinlock support */
    /* (In the actual build, SpinlockSemaInit() is called here) */
    (void) progname;
}

/* -----------------------------------------------------------------------
 * get_restricted_token — Windows security token (no-op on Android)
 * ----------------------------------------------------------------------- */
static void
get_restricted_token(void)
{
    /* No-op on Android / Linux */
}

/* -----------------------------------------------------------------------
 * Memory allocation stubs for frontend code compiled into the same TU.
 * These delegate to C stdlib so frontend tools do not need palloc.
 * ----------------------------------------------------------------------- */
void *
pg_malloc(size_t size)
{
    void *ptr = malloc(size);
    if (!ptr)
        abort();
    return ptr;
}

void *
pg_realloc(void *ptr, size_t size)
{
    void *newptr = realloc(ptr, size);
    if (!newptr)
        abort();
    return newptr;
}

char *
pg_strdup(const char *str)
{
    char *result = strdup(str);
    if (!result)
        abort();
    return result;
}

void
pg_free(void *ptr)
{
    free(ptr);
}

/* -----------------------------------------------------------------------
 * simple_prompt — returns empty string (no interactive prompting in JNI)
 * ----------------------------------------------------------------------- */
char *
simple_prompt(const char *prompt, bool echo)
{
    (void) prompt;
    (void) echo;
    return pg_strdup("");
}

/* -----------------------------------------------------------------------
 * ProcessStartupPacket stub (multi-process path, not used in single mode)
 * ----------------------------------------------------------------------- */
int
ProcessStartupPacket(struct Port *port, bool ssl_done, bool gss_done)
{
    (void) port; (void) ssl_done; (void) gss_done;
    return STATUS_OK;
}

