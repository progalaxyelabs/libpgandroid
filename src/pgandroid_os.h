/*
 * pgandroid/pgandroid_os.h
 *
 * OS-level overrides for pgandroid.
 *
 * Primary purpose: intercept popen()/pclose() so that initdb's bootstrap
 * commands (sent via popen("postgres --boot ...") and popen("postgres --single ..."))
 * are redirected to temp files, and then the appropriate backend entry point
 * is called to actually execute those commands.
 *
 * How this works:
 *
 *  Stage 0 (bootstrap):
 *    initdb calls popen("postgres --boot ...", "w")
 *    → pgandroid_popen() creates $PGDATA/pgandroid_pipe_boot for writing
 *    initdb writes BKI bootstrap commands to this file
 *    initdb calls pclose()
 *    → pgandroid_pclose() opens the boot file for reading,
 *      redirects stdin, and calls BootstrapModeMain(--boot, ...)
 *
 *  Stage 1 (single-user):
 *    initdb calls popen("postgres --single ...", "w")
 *    → pgandroid_popen() creates $PGDATA/pgandroid_pipe_single for writing
 *    initdb writes SQL setup commands to this file
 *    initdb calls pclose()
 *    → pgandroid_pclose() opens the single file for reading,
 *      redirects stdin, and calls PostgresSingleUserMain(--single, ...)
 *
 * pgandroid_popen() and pgandroid_pclose() are defined in pgandroid_main.c
 * (which has access to backend headers) and declared extern here.
 *
 * Include this header BEFORE any PostgreSQL headers.
 */

#pragma once

#include <stdio.h>

/* -----------------------------------------------------------------------
 * Pipe interception — declared here, defined in pgandroid_main.c.
 * These are NOT static so they are visible across TUs.
 * ----------------------------------------------------------------------- */
extern FILE *pgandroid_popen(const char *command, const char *type);
extern int   pgandroid_pclose(FILE *stream);

/* -----------------------------------------------------------------------
 * Override popen/pclose with our intercept functions.
 * Must be included before any headers that pull in <stdio.h>.
 * ----------------------------------------------------------------------- */
#define popen(cmd, type)  pgandroid_popen((cmd), (type))
#define pclose(stream)    pgandroid_pclose(stream)
