/*
 * pgandroid/pgandroid_initdb.c  (v2 — direct call approach)
 *
 * initdb integration for pgandroid.
 *
 * Compiled as a SEPARATE frontend translation unit (with -DFRONTEND and
 * frontend include paths).  This avoids the frontend/backend header conflict
 * that arises when initdb.c (which starts with #include "postgres_fe.h") is
 * compiled in the same translation unit as postgres.h (backend).
 *
 * v2 key changes from v1:
 *  - popen/pclose intercepts are defined INLINE here (no pgandroid_os.h)
 *  - pclose calls pgandroid_call_postgres() which parses the command string
 *    and calls pgandroid_postgres() (defined in pgandroid_main.c)
 *  - pgandroid_reset_state() is called between phases (defined in pgandroid_main.c)
 *  - No named pipe files under PGDATA — uses tmpfile() instead
 *
 * Build flags required:
 *   -DFRONTEND
 *   -D__ANDROID_PGANDROID__
 */

/* -----------------------------------------------------------------------
 * Android logcat logging
 * ----------------------------------------------------------------------- */
#include <android/log.h>
#define PGANDROID_TAG  "pgandroid"
#define PGANDROID_LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  PGANDROID_TAG, __VA_ARGS__)
#define PGANDROID_LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, PGANDROID_TAG, __VA_ARGS__)

/* -----------------------------------------------------------------------
 * Standard headers
 * ----------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <setjmp.h>
#include <getopt.h>
#include <fcntl.h>

/* -----------------------------------------------------------------------
 * Extern declarations for functions defined in pgandroid_main.c (backend TU).
 * These are linked at .so link time.
 * ----------------------------------------------------------------------- */
extern void pgandroid_postgres(int argc, char *argv[]);
extern void pgandroid_reset_state(void);

/* -----------------------------------------------------------------------
 * exit() / _exit() interception for initdb.
 *
 * initdb.c calls exit() when it finishes (success or failure).  In an
 * embedded JNI library, calling exit() tears down the ART runtime.
 * We redirect to a longjmp() back into pgandroid_run_initdb().
 * ----------------------------------------------------------------------- */
static jmp_buf   pgandroid_initdb_jmpbuf;
static int       pgandroid_initdb_exit_code;
static volatile int pgandroid_initdb_jmpbuf_ready = 0;

static void __attribute__((noreturn))
pgandroid_initdb_exit(int code)
{
    PGANDROID_LOGI("pgandroid_initdb_exit: caught exit(%d), jmpbuf_ready=%d",
                   code, pgandroid_initdb_jmpbuf_ready);
    pgandroid_initdb_exit_code = code;
    if (pgandroid_initdb_jmpbuf_ready)
        longjmp(pgandroid_initdb_jmpbuf, 1);
    PGANDROID_LOGE("pgandroid_initdb_exit: jmpbuf NOT ready, aborting!");
    _Exit(99);
}

static int
pgandroid_initdb_atexit(void (*func)(void))
{
    (void) func;
    return 0;
}

/* -----------------------------------------------------------------------
 * Temp directory setup
 * ----------------------------------------------------------------------- */
static inline void
pgandroid_initdb_setup_tmpdir(const char *datadir)
{
    static char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/../tmp", datadir);
    mkdir(tmpdir, 0700);
    setenv("TMPDIR", tmpdir, 1);
    setenv("TEMP",   tmpdir, 1);
    setenv("TMP",    tmpdir, 1);
}

/* -----------------------------------------------------------------------
 * popen/pclose interception (v2 — inline, no pgandroid_os.h)
 *
 * initdb uses popen("postgres --boot ...", "w") and
 * popen("postgres --single ...", "w") to run bootstrap queries.
 *
 * Our popen() creates a tmpfile for writing. initdb writes BKI/SQL to it.
 * Our pclose() rewinds the tmpfile, redirects stdin to it, parses the
 * command string into argv, and calls pgandroid_postgres(argc, argv).
 * ----------------------------------------------------------------------- */

/* Saved command string from the most recent popen call */
static char pgandroid_saved_cmd[4096];

/* -----------------------------------------------------------------------
 * pgandroid_call_postgres — parse a command string into argc/argv and
 * call pgandroid_postgres().
 *
 * Command strings from initdb look like:
 *   "/path/to/postgres" --boot -j -X 1048576
 *   "/path/to/postgres" --single -j template1
 *
 * We do simple whitespace splitting (quotes around the binary path are
 * stripped). This is sufficient for initdb's well-formed commands.
 * ----------------------------------------------------------------------- */
static void
pgandroid_call_postgres(const char *cmd)
{
    char  buf[4096];
    char *argv[64];
    int   argc = 0;
    char *p;

    strlcpy(buf, cmd, sizeof(buf));
    p = buf;

    while (*p && argc < 63)
    {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        /* Handle quoted argument */
        if (*p == '"')
        {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
                *p++ = '\0';
        }
        else
        {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            if (*p)
                *p++ = '\0';
        }
    }
    argv[argc] = NULL;

    if (argc < 2)
    {
        PGANDROID_LOGE("pgandroid_call_postgres: too few args in cmd: %s", cmd);
        return;
    }

    /*
     * Skip any redirect to /dev/null at the end of the command.
     * initdb appends ">%s" (DEVNULL) which we don't need.
     */
    while (argc > 0 && argv[argc-1][0] == '>')
        argv[--argc] = NULL;

    PGANDROID_LOGI("pgandroid_call_postgres: argc=%d, argv[1]=%s", argc,
                   argc > 1 ? argv[1] : "(none)");
    pgandroid_postgres(argc, argv);
}

/*
 * Our popen replacement — creates a tmpfile for initdb to write to.
 */
static FILE *
pgandroid_popen_v2(const char *command, const char *type)
{
    FILE *fp;

    (void) type;  /* always "w" from initdb */

    strlcpy(pgandroid_saved_cmd, command, sizeof(pgandroid_saved_cmd));
    fp = tmpfile();
    if (!fp)
        PGANDROID_LOGE("pgandroid_popen_v2: tmpfile() failed: %s", strerror(errno));
    else
        PGANDROID_LOGI("pgandroid_popen_v2: created tmpfile for cmd: %.80s...", command);
    return fp;
}

/*
 * Our pclose replacement — reads the tmpfile, redirects stdin, calls postgres.
 */
static int
pgandroid_pclose_v2(FILE *stream)
{
    int saved_stdin;

    if (!stream)
        return 0;

    /* Flush and rewind — the data initdb wrote is in this tmpfile */
    fflush(stream);
    rewind(stream);

    /* Redirect stdin to the tmpfile */
    saved_stdin = dup(STDIN_FILENO);
    dup2(fileno(stream), STDIN_FILENO);

    /* Parse command string and call pgandroid_postgres() */
    pgandroid_call_postgres(pgandroid_saved_cmd);

    /* Restore stdin */
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    fclose(stream);

    /* Reset state for next phase */
    pgandroid_reset_state();

    return 0;  /* 0 = success */
}

/* Override popen/pclose within this TU so initdb.c uses our functions */
#define popen(cmd, type)  pgandroid_popen_v2((cmd), (type))
#define pclose(stream)    pgandroid_pclose_v2(stream)

/* -----------------------------------------------------------------------
 * Locale setup for initdb
 * ----------------------------------------------------------------------- */
static void
pgandroid_setup_locale(void)
{
    setenv("LANG",        "C.UTF-8",    1);
    setenv("LC_ALL",      "C",          1);
    setenv("LC_COLLATE",  "C",          1);
    setenv("LC_CTYPE",    "C.UTF-8",    1);
    setenv("LC_MESSAGES", "C",          1);
    setenv("LC_MONETARY", "C",          1);
    setenv("LC_NUMERIC",  "C",          1);
    setenv("LC_TIME",     "C",          1);
}

/* -----------------------------------------------------------------------
 * find_other_exec stub — no postgres binary on Android.
 * Return success with a fake path so initdb proceeds.
 * ----------------------------------------------------------------------- */
static int
pgandroid_find_other_exec(const char *argv0, const char *target,
                          const char *versionstr, char *retpath)
{
    (void) argv0;
    (void) versionstr;
    snprintf(retpath, PATH_MAX, "/pgandroid/bin/%s", target);
    PGANDROID_LOGI("find_other_exec: stub returning fake path %s", retpath);
    return 0;
}

/* -----------------------------------------------------------------------
 * Preprocessor overrides before including initdb.c
 * ----------------------------------------------------------------------- */

/* Redirect exit/_exit/abort → longjmp so we don't tear down ART */
#define exit(code)  pgandroid_initdb_exit(code)
#define _exit(code) pgandroid_initdb_exit(code)
#define abort()     pgandroid_initdb_exit(99)
#define atexit(fn)  pgandroid_initdb_atexit(fn)

/* Redirect find_other_exec → our stub */
#define find_other_exec pgandroid_find_other_exec

/* Rename main → initdb_main to avoid symbol conflict */
#define main initdb_main

/* initdb.c's local 'dynamic_shared_memory_type' conflicts with the backend GUC */
#define dynamic_shared_memory_type initdb_dsm_type

/* -----------------------------------------------------------------------
 * Frontend memory allocation.
 *
 * The .so links both frontend and backend code. With --allow-multiple-definition,
 * the backend palloc/pfree (MemoryContext-based) wins over the frontend
 * (malloc-based). initdb calls psprintf → palloc → backend MemoryContext,
 * then free() → SIGABRT (Scudo detects misaligned pointer).
 *
 * Fix: Include fe_memutils.c and psprintf.c with renamed symbols so initdb's
 * allocation calls route to the malloc-based frontend versions.
 * ----------------------------------------------------------------------- */
#define pg_malloc_internal  initdb_fe_pg_malloc_internal
#define pg_malloc           initdb_fe_pg_malloc
#define pg_malloc0          initdb_fe_pg_malloc0
#define pg_malloc_extended  initdb_fe_pg_malloc_extended
#define pg_realloc          initdb_fe_pg_realloc
#define pg_strdup           initdb_fe_pg_strdup
#define pg_free             initdb_fe_pg_free
#define palloc              initdb_fe_palloc
#define palloc0             initdb_fe_palloc0
#define palloc_extended     initdb_fe_palloc_extended
#define pfree               initdb_fe_pfree
#define pstrdup             initdb_fe_pstrdup
#define pnstrdup            initdb_fe_pnstrdup
#define repalloc            initdb_fe_repalloc
#define psprintf            initdb_fe_psprintf
#define pvsnprintf          initdb_fe_pvsnprintf

#include "common/fe_memutils.c"
#include "common/psprintf.c"

/* -----------------------------------------------------------------------
 * Include initdb.c at file scope (single-TU frontend compilation)
 * ----------------------------------------------------------------------- */
#include "bin/initdb/initdb.c"

/* Clean up macro overrides */
#undef dynamic_shared_memory_type
#undef main
#undef find_other_exec
#undef atexit
#undef abort
#undef _exit
#undef exit
#undef pclose
#undef popen

/* -----------------------------------------------------------------------
 * pgandroid_run_initdb — run initdb in-process
 *
 * Equivalent to: initdb -D datadir -E UTF8 --locale=C -U postgres -A trust
 *
 * Returns 0 on success, non-zero on failure.
 * ----------------------------------------------------------------------- */
int
pgandroid_run_initdb(const char *datadir)
{
    static char share_path_buf[PATH_MAX];
    snprintf(share_path_buf, sizeof(share_path_buf), "%s/../share", datadir);
    PGANDROID_LOGI("pgandroid_run_initdb: share_path=%s", share_path_buf);

    const char *initdb_argv[] = {
        "initdb",
        "-D",        datadir,
        "-E",        "UTF8",
        "--locale=C",
        "-U",        "postgres",
        "-A",        "trust",
        "--no-sync",
        "-L",        share_path_buf,
        NULL
    };
    int initdb_argc = 13;

    pgandroid_setup_locale();
    pgandroid_initdb_setup_tmpdir(datadir);

    PGANDROID_LOGI("pgandroid_run_initdb: starting initdb in %s, geteuid=%d",
                   datadir, (int)geteuid());

    /* Verify share files exist before calling initdb */
    {
        struct stat st;
        static const char *share_files[] = {
            "postgres.bki", "pg_hba.conf.sample", "pg_ident.conf.sample",
            "postgresql.conf.sample", "snowball_create.sql",
            "information_schema.sql", "sql_features.txt",
            "system_constraints.sql", "system_functions.sql",
            "system_views.sql", NULL
        };
        char fpath[PATH_MAX];
        for (int fi = 0; share_files[fi] != NULL; fi++)
        {
            snprintf(fpath, sizeof(fpath), "%s/../share/%s", datadir, share_files[fi]);
            int ok = (stat(fpath, &st) == 0 && S_ISREG(st.st_mode));
            if (!ok)
                PGANDROID_LOGE("pgandroid_run_initdb: MISSING share file: %s (errno=%d)",
                               fpath, errno);
        }
        PGANDROID_LOGI("pgandroid_run_initdb: share file scan done");
    }

    /* Reset getopt state */
    optind = 0;

    /* Arm the longjmp target for exit() interception */
    pgandroid_initdb_exit_code = 0;
    pgandroid_initdb_jmpbuf_ready = 1;
    if (setjmp(pgandroid_initdb_jmpbuf) == 0)
    {
        initdb_main(initdb_argc, (char **)(uintptr_t)initdb_argv);
        /* initdb_main should not return — it always calls exit() */
    }
    pgandroid_initdb_jmpbuf_ready = 0;

    int rc = pgandroid_initdb_exit_code;
    PGANDROID_LOGI("pgandroid_run_initdb: initdb exited with code %d", rc);
    return rc;
}
