/*
 * pgandroid/pgandroid_initdb.c
 *
 * initdb integration for pgandroid. Mirrors pglite-wasm/pgl_initdb.c.
 *
 * Compiled as a SEPARATE frontend translation unit (with -DFRONTEND and
 * frontend include paths).  This avoids the frontend/backend header conflict
 * that arises when initdb.c (which starts with #include "postgres_fe.h") is
 * compiled in the same translation unit as postgres.h (backend).
 *
 * What this does:
 *  1. Overrides pg_chmod() — on Android we use real chmod (unlike WASM which
 *     makes it a no-op), but we do want to skip the root-ownership check.
 *  2. Includes bin/initdb/initdb.c directly (single-TU frontend compilation).
 *  3. Provides locale override stubs needed by initdb.
 *  4. Exports pgandroid_run_initdb(datadir) which is called by pgandroid_init().
 *
 * Directory layout expectation:
 *   pgandroid_run_initdb() is called with the app's datadir.
 *   It creates a PostgreSQL cluster there with:
 *     - UTF-8 encoding
 *     - C.UTF-8 locale (or C if UTF-8 is not available)
 *     - "postgres" superuser (no password for embedded use)
 *     - auth method: trust (local connections only, no network)
 *
 * Prerequisites (must be called before this):
 *   setenv("PGDATA", datadir, 1)
 *
 * Build flags required:
 *   -DFRONTEND
 *   -D__ANDROID_PGANDROID__
 *   -I.../src/include
 *   -I.../src/interfaces/libpq
 *   -I.../src/bin/initdb
 */

/* -----------------------------------------------------------------------
 * Android logcat logging — needed since PGANDROID_LOGI/E are not inherited
 * from pgandroid_main.c in separate-TU compilation.
 * ----------------------------------------------------------------------- */
#include <android/log.h>
#define PGANDROID_TAG  "pgandroid"
#define PGANDROID_LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  PGANDROID_TAG, __VA_ARGS__)
#define PGANDROID_LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, PGANDROID_TAG, __VA_ARGS__)

/* -----------------------------------------------------------------------
 * pgandroid_setup_tmpdir — create PGDATA/tmp and export TMPDIR/TEMP/TMP.
 * Copied from android_pgandroid.h (cannot include that header in frontend TU
 * because it pulls in backend-only stubs).
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
#include <getopt.h>     /* for optind (reset before each initdb_main call) */
#include <fcntl.h>      /* for O_WRONLY, O_CREAT, O_TRUNC, O_RDONLY, open() */

/* -----------------------------------------------------------------------
 * exit() / _exit() interception for initdb.
 *
 * initdb.c calls exit() when it finishes (success or failure).  In an
 * embedded JNI library, calling exit() tears down the ART runtime via
 * atexit() handlers, destroying JVM-internal mutexes.  The Android main
 * thread then crashes when it tries to lock those destroyed mutexes.
 *
 * We redirect exit() and _exit() to a longjmp() back into
 * pgandroid_run_initdb(), which uses setjmp() to catch the jump.
 * atexit() is made a no-op so that ART cleanup handlers are never called.
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
    /* setjmp not set up yet — this should never happen */
    PGANDROID_LOGE("pgandroid_initdb_exit: jmpbuf NOT ready, aborting!");
    abort();
}

static int
pgandroid_initdb_atexit(void (*func)(void))
{
    /* no-op: suppress atexit() registration from initdb so ART is not torn down */
    (void) func;
    return 0;
}

static inline void
pgandroid_initdb_setup_tmpdir(const char *datadir)
{
    /*
     * Create the tmp directory OUTSIDE the data directory (at the pgandroid
     * root level: datadir/../tmp).  initdb calls pg_check_dir() on pg_data
     * and refuses to initialise if the directory is non-empty.  Placing tmp
     * inside pg_data would make it non-empty and cause a spurious exit(1).
     *
     * datadir   = .../pgandroid/<dbName>/
     * tmpdir    = .../pgandroid/tmp/       (sibling of <dbName>/ and share/)
     */
    static char tmpdir[PATH_MAX];
    snprintf(tmpdir, sizeof(tmpdir), "%s/../tmp", datadir);
    mkdir(tmpdir, 0700);
    setenv("TMPDIR", tmpdir, 1);
    setenv("TEMP",   tmpdir, 1);
    setenv("TMP",    tmpdir, 1);
}

/* -----------------------------------------------------------------------
 * popen/pclose interception for initdb bootstrap.
 * initdb uses popen("postgres --boot") and popen("postgres --single") to
 * run bootstrap queries.  We redirect these to temporary files under PGDATA.
 * ----------------------------------------------------------------------- */
#include "pgandroid_os.h"

/* -----------------------------------------------------------------------
 * pg_chmod override
 *
 * PGlite makes pg_chmod a no-op because the WASM VFS does not support modes.
 * Android has real chmod, so we keep it functional.
 * However, we skip the root-ownership check that initdb performs (Android
 * apps run as unprivileged UIDs, not root).
 * ----------------------------------------------------------------------- */
/* pg_chmod is defined in src/bin/initdb/initdb.c as a wrapper around chmod().
 * We do not override it here — the real chmod works fine on Android.
 * The root-ownership check bypass is handled by the template (patch 004). */
/* -----------------------------------------------------------------------
 * Locale stubs for initdb
 *
 * initdb calls setlocale(LC_ALL, "") and pg_perm_setlocale() to detect
 * the system locale.  On Android:
 *   - setlocale() works but the locale may be "C" or something platform-specific
 *   - We override the locale detection to always use C.UTF-8
 * ----------------------------------------------------------------------- */
static const char *pgandroid_default_locale = "C";
static const char *pgandroid_default_encoding = "UTF8";
/* Override the locale so initdb does not try to use a non-existent locale */
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
 * Include initdb.c at file scope (single-TU compilation).
 *
 * Must be at file scope (NOT inside a function body) because initdb.c
 * transitively includes PostgreSQL headers with static inline function
 * definitions (rmgr.h, xlogreader.h, xlog_internal.h, etc.) which the
 * C compiler rejects when they appear inside a function body.
 *
 * The main() symbol is renamed to initdb_main() to avoid conflicts with
 * any other main() in the combined link unit.
 *
 * pgandroid_os.h (included via pgandroid_main.c → android_pgandroid.h)
 * intercepts popen() calls that initdb.c uses for "postgres --boot".
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * find_other_exec stub for Android.
 *
 * initdb calls find_other_exec(argv0, "postgres", ...) in setup_bin_paths()
 * to locate the postgres backend binary.  On Android there is no postgres
 * binary — popen/pclose are intercepted to call BootstrapModeMain and
 * PostgresSingleUserMain directly.  Return 0 (success) with a fake path
 * so initdb proceeds without searching for a binary that does not exist.
 * ----------------------------------------------------------------------- */
static int
pgandroid_find_other_exec(const char *argv0, const char *target,
                          const char *versionstr, char *retpath)
{
    (void) argv0;
    (void) versionstr;
    /* popen/pclose are intercepted — no real postgres binary is needed */
    snprintf(retpath, PATH_MAX, "/pgandroid/bin/%s", target);
    PGANDROID_LOGI("find_other_exec: stub returning fake path %s", retpath);
    return 0;  /* 0 = success */
}

/* Redirect exit()/_exit() → pgandroid_initdb_exit (longjmp) so we don't
 * tear down the ART JVM when initdb finishes.
 * Redirect atexit() → no-op so ART cleanup handlers are never registered. */
#define exit(code)  pgandroid_initdb_exit(code)
#define _exit(code) pgandroid_initdb_exit(code)
#define abort()     pgandroid_initdb_exit(99)
#define atexit(fn)  pgandroid_initdb_atexit(fn)

/* Redirect find_other_exec → our stub (no real postgres binary on Android) */
#define find_other_exec pgandroid_find_other_exec

/* pg_log_generic_v logging: handled via logging.c Android hook (see below) */

#define main initdb_main
/* initdb.c declares a local 'static const char *dynamic_shared_memory_type'
 * which conflicts with the backend's 'int dynamic_shared_memory_type' GUC
 * (declared in dsm_impl.h).  Rename the initdb local to avoid the clash. */
#define dynamic_shared_memory_type initdb_dsm_type

/* -----------------------------------------------------------------------
 * Frontend memory allocation for initdb.
 *
 * Problem: The .so links both frontend and backend code.  Functions like
 * palloc, pfree, psprintf, pg_malloc exist in both fe_memutils.c (frontend,
 * uses malloc) and mcxt.c (backend, uses MemoryContext).  With the linker
 * flag --allow-multiple-definition, the backend versions win because backend
 * objects are linked first.  When initdb.c calls psprintf() -> palloc() ->
 * backend MemoryContext, then free() on the result -> SIGABRT (Scudo detects
 * misaligned pointer -- it's not a malloc pointer).
 *
 * Fix: Include fe_memutils.c and psprintf.c directly in this TU with
 * unique symbol names.  The renames stay active for initdb.c, so all its
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

#include "bin/initdb/initdb.c"
#undef dynamic_shared_memory_type
#undef main
#undef find_other_exec

#undef atexit
#undef _exit
#undef exit

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
    /*
     * Share directory: one level above the per-database data directory.
     *
     * PgDatabase.kt extracts APK assets/pgandroid/share/ to:
     *   context.filesDir/pgandroid/share/
     * The per-database data directory is:
     *   context.filesDir/pgandroid/<dbName>/
     *
     * So share_path = datadir + "/../share" which resolves to:
     *   context.filesDir/pgandroid/share/
     *
     * We pass -L <share_path> to initdb so that check_input() can find
     * postgres.bki, pg_hba.conf.sample, etc.  Without -L, initdb derives
     * share_path from the (fake) postgres binary path and the files are not found.
     */
    static char share_path_buf[PATH_MAX];
    snprintf(share_path_buf, sizeof(share_path_buf), "%s/../share", datadir);
    /* Note: initdb calls canonicalize_path() on share_path internally,
     * collapsing the ".." component.  No need to resolve it here. */
    PGANDROID_LOGI("pgandroid_run_initdb: share_path=%s", share_path_buf);

    /*
     * Argument list for initdb (mirrors the WASM initdb invocation in PGlite).
     * Key choices for embedded Android use:
     *   -E UTF8       — UTF-8 encoding (essential for modern apps)
     *   --locale=C    — C locale avoids locale-dependent collation issues
     *   -U postgres   — superuser name (matches ANDROID_USERNAME)
     *   -A trust      — trust authentication (no password for local JNI use)
     *   --no-sync     — skip fsync during initdb for speed (data is fresh)
     *   -D datadir    — target data directory
     *   -L share_path — location of postgres.bki and other bootstrap files
     *                   (extracted from APK assets by PgDatabase.kt)
     */
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
    int initdb_argc = 13;   /* count of non-NULL entries above */

    /* Set up locale overrides before running initdb */
    pgandroid_setup_locale();

    /*
     * Set up temp directory OUTSIDE the data directory.
     * tmp is placed at datadir/../tmp (sibling of the data dir and share/).
     * It must NOT be inside datadir: initdb calls pg_check_dir(pg_data) and
     * refuses to initialise if the directory is non-empty.
     * initdb itself creates and populates pg_data.
     */
    pgandroid_initdb_setup_tmpdir(datadir);

    PGANDROID_LOGI("pgandroid_run_initdb: starting initdb in %s, geteuid=%d",
                   datadir, (int)geteuid());

    /* Diagnostic: verify all 10 share files exist before calling initdb.
     * These are the files check_input() validates in setup_data_file_paths(). */
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

    /* Reset getopt state so initdb argument parsing starts fresh each call.
     * Setting optind=0 causes GNU/bionic getopt_long to reinitialise itself,
     * avoiding stale state from a previous (failed) initdb run. */
    optind = 0;

    /* Redirect stderr to logcat by intercepting pg_log_generic_v.
     * Instead of file redirect (which can cause SIGABRT if dup2 interferes
     * with initdb's stdio), we hook into PostgreSQL's logging system.
     * For now, skip stderr redirect entirely and add diagnostic logging
     * to catch the actual error message. */
    static char initdb_stderr_path[PATH_MAX];
    initdb_stderr_path[0] = '\0';  /* disable file redirect */
    int saved_stderr_fd = -1;
    int stderr_file_fd = -1;
    /* stderr redirect disabled — dup2 during initdb causes SIGABRT.
     * TODO: re-enable once initdb runs successfully. */

    /* Arm the longjmp target so pgandroid_initdb_exit() can jump here */
    pgandroid_initdb_exit_code = 0;
    pgandroid_initdb_jmpbuf_ready = 1;
    if (setjmp(pgandroid_initdb_jmpbuf) == 0)
    {
        /* Normal path: initdb runs and eventually calls exit() */
        initdb_main(initdb_argc, (char **)(uintptr_t)initdb_argv);
        /* initdb_main() should not return — it always calls exit() */
    }
    /* Arrived here via longjmp from pgandroid_initdb_exit() */
    pgandroid_initdb_jmpbuf_ready = 0;

    /* Restore stderr and log captured initdb output */
    if (saved_stderr_fd >= 0)
    {
        fflush(stderr);
        dup2(saved_stderr_fd, STDERR_FILENO);
        close(saved_stderr_fd);
    }
    if (stderr_file_fd >= 0)
    {
        close(stderr_file_fd);
        /* Read and log the captured stderr content */
        int rfd = open(initdb_stderr_path, O_RDONLY);
        if (rfd >= 0)
        {
            char buf[4096];
            ssize_t n;
            while ((n = read(rfd, buf, sizeof(buf) - 1)) > 0)
            {
                buf[n] = '\0';
                PGANDROID_LOGE("initdb stderr: %s", buf);
            }
            close(rfd);
        }
    }

    int rc = pgandroid_initdb_exit_code;
    PGANDROID_LOGI("pgandroid_run_initdb: initdb exited with code %d", rc);
    return rc;
}
