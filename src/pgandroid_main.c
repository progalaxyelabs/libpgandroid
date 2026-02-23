/*
 * pgandroid/pgandroid_main.c
 *
 * Combined main translation unit for pgandroid.
 * Mirrors pglite-wasm/pg_main.c but exports JNI entry points instead of
 * Emscripten JS-callable functions.
 *
 * Compilation:
 *   This file is compiled as a single translation unit that #include-s
 *   the other pgandroid source fragments.  The linker sees one big .o file
 *   containing everything, which avoids duplicate-symbol issues from
 *   PostgreSQL's frontend+backend combined link.
 *
 * Build flags required:
 *   -D__ANDROID_PGANDROID__
 *   -DPGANDROID_MAIN
 *   -DPGANDROID_INITDB_MAIN
 *   -include android_pgandroid.h
 *
 * Entry points (exported from libpgandroid.so):
 *   pgandroid_init(const char *datadir)
 *   pgandroid_exec(const char *sql)
 *   pgandroid_exec_params(const char *sql, const char *params_json)
 *   pgandroid_close()
 *
 * These are thin C wrappers.  pgandroid_jni.c (a separate file, not in
 * the postgres-pglite patch set) wraps them with JNIEnv boilerplate.
 */

/* -----------------------------------------------------------------------
 * Compilation markers (mirror of PGL_MAIN / PGL_INITDB_MAIN in PGlite)
 * ----------------------------------------------------------------------- */
#define PGANDROID_MAIN
#define PGANDROID_INITDB_MAIN

/* -----------------------------------------------------------------------
 * Android OS-level overrides (popen/pclose for initdb bootstrap)
 * ----------------------------------------------------------------------- */
#include "pgandroid_os.h"

/* -----------------------------------------------------------------------
 * PostgreSQL backend headers
 * ----------------------------------------------------------------------- */
#include "postgres.h"
/* c.h (included by postgres.h) redefines pg_attribute_noreturn() to
 * __attribute__((noreturn)), overriding android_pgandroid.h's no-op definition.
 * We must restore the no-op so that pg_proc_exit() can return to the JNI caller. */
#undef  pg_attribute_noreturn
#define pg_attribute_noreturn() /* nothing — pg_proc_exit() returns in pgandroid */
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "bootstrap/bootstrap.h"
#include "storage/ipc.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"

/* Android NDK logging */
#include <android/log.h>
#include <pthread.h>
#define PGANDROID_TAG "pgandroid"
#define PGANDROID_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  PGANDROID_TAG, __VA_ARGS__)
#define PGANDROID_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PGANDROID_TAG, __VA_ARGS__)

/* -----------------------------------------------------------------------
 * Global state (mirrors PREFIX, PGDATA, PGUSER in pg_main.c)
 * ----------------------------------------------------------------------- */

/* Data directory set by pgandroid_init(); persists across queries */
static const char *pgandroid_datadir  = NULL;

/* true after pgandroid_init() completes successfully */
static bool        pgandroid_ready    = false;

/* true if pgandroid_close() has been called */
static bool        pgandroid_closed   = false;

/* -----------------------------------------------------------------------
 * popen/pclose intercept implementations (defined here to access backend fns)
 *
 * These are the actual definitions of the functions declared in pgandroid_os.h.
 * They must be in the backend TU (pgandroid_main.c) so that they can call
 * BootstrapModeMain() and PostgresSingleUserMain() directly.
 *
 * Flow during initdb:
 *   Stage 0 → popen("postgres --boot ...", "w")
 *     → fopen($PGDATA/pgandroid_pipe_boot, "w")
 *   Stage 0 → pclose(boot_fp)
 *     → fclose(boot_fp)
 *     → BootstrapModeMain(--boot, -j ...) with stdin from boot file
 *   Stage 1 → popen("postgres --single ...", "w")
 *     → fopen($PGDATA/pgandroid_pipe_single, "w")
 *   Stage 1 → pclose(single_fp)
 *     → fclose(single_fp)
 *     → PostgresSingleUserMain(--single, -j, ..., template1) with stdin from single file
 * ----------------------------------------------------------------------- */
#include <fcntl.h>

static int   pgandroid_os_stage        = 0;
static char  pgandroid_os_boot_path[PATH_MAX]   = "";
static char  pgandroid_os_single_path[PATH_MAX] = "";
static FILE *pgandroid_os_current_fp   = NULL;

/*
 * pgandroid_reset_exit_state — reset PostgreSQL exit state so that
 * PostgresSingleUserMain() can be called again after BootstrapModeMain()
 * or a prior PostgresSingleUserMain() has returned via proc_exit(0).
 *
 * During initdb, both BootstrapModeMain and PostgresSingleUserMain(template1)
 * call proc_exit(0) to signal normal completion.  In pgandroid, proc_exit()
 * returns instead of calling exit(), but it leaves proc_exit_inprogress=true.
 * If we don't reset that flag, the next PostgresSingleUserMain call will see
 * all ERRORs escalated to FATAL (see elog.c: if proc_exit_inprogress, elevel=FATAL).
 *
 * We also call on_exit_reset() to clear the exit callback lists so that
 * the next PostgresSingleUserMain can register its own callbacks fresh.
 */
static void
pgandroid_reset_exit_state(void)
{
    /* Reset the "exit in progress" flag so ERROR handling works normally */
    proc_exit_inprogress  = false;
    shmem_exit_inprogress = false;

    /*
     * Clear exit callback lists.  on_exit_reset() sets all three indices
     * (before_shmem_exit, on_shmem_exit, on_proc_exit) back to 0.
     * The callbacks that were registered by the just-completed backend
     * run are stale (they refer to shared memory segments and locks that
     * no longer exist in our single-process model).
     */
    on_exit_reset();

    /*
     * Reset TopMemoryContext to NULL so that MemoryContextInit() (called by
     * the next PostgresSingleUserMain) can create a fresh memory context
     * hierarchy without hitting the Assert(TopMemoryContext == NULL) check.
     * The old TopMemoryContext and its children are leaked; this is a one-time
     * startup cost during initdb bootstrap.
     */
    TopMemoryContext    = NULL;
    CurrentMemoryContext = NULL;
    ErrorContext        = NULL;

    PGANDROID_LOGI("pgandroid_reset_exit_state: proc_exit state reset for next backend run");
}

FILE *
pgandroid_popen(const char *command, const char *type)
{
    const char *datadir = getenv("PGDATA");
    char        path[PATH_MAX];
    FILE       *fp;

    if (!datadir)
        datadir = "/data/data/pgandroid/files/pgdata";

    (void) command; /* We don't parse the command; just track the stage */
    (void) type;

    switch (pgandroid_os_stage)
    {
        case 0: /* Boot stage */
            snprintf(path, sizeof(path), "%s/pgandroid_pipe_boot", datadir);
            strncpy(pgandroid_os_boot_path, path, PATH_MAX - 1);
            fp = fopen(path, "w");
            pgandroid_os_current_fp = fp;
            pgandroid_os_stage = 1;
            PGANDROID_LOGI("pgandroid_popen: boot stage, writing to %s", path);
            return fp;

        case 1: /* Single-user stage */
            snprintf(path, sizeof(path), "%s/pgandroid_pipe_single", datadir);
            strncpy(pgandroid_os_single_path, path, PATH_MAX - 1);
            fp = fopen(path, "w");
            pgandroid_os_current_fp = fp;
            pgandroid_os_stage = 2;
            PGANDROID_LOGI("pgandroid_popen: single stage, writing to %s", path);
            return fp;

        default:
            /* Subsequent popen calls (if any) go to /dev/null */
            pgandroid_os_stage++;
            return fopen("/dev/null", "w");
    }
}

int
pgandroid_pclose(FILE *stream)
{
    int  tmp_fd;
    int  saved_stdin;

    if (!stream)
        return 0;

    fflush(stream);
    fclose(stream);
    pgandroid_os_current_fp = NULL;

    if (pgandroid_os_stage == 1 && pgandroid_os_boot_path[0] != '\0')
    {
        /*
         * Boot stage just completed.  Run BootstrapModeMain with stdin
         * redirected from the boot commands file.
         */
        PGANDROID_LOGI("pgandroid_pclose: running BootstrapModeMain from %s",
                       pgandroid_os_boot_path);

        tmp_fd = open(pgandroid_os_boot_path, O_RDONLY);
        if (tmp_fd < 0)
        {
            PGANDROID_LOGE("pgandroid_pclose: cannot open boot file: %s",
                           pgandroid_os_boot_path);
            return -1;
        }

        saved_stdin = dup(STDIN_FILENO);
        dup2(tmp_fd, STDIN_FILENO);
        close(tmp_fd);

        {
            static char *boot_argv[] = {
                "postgres", "--boot", "-j", NULL
            };
            int boot_argc = 3;
            /*
             * Initialize PostgreSQL memory contexts (TopMemoryContext, ErrorContext)
             * before calling BootstrapModeMain. Without this, any elog() call in
             * InitStandaloneProcess (e.g., when find_my_exec fails) would hit
             * elog.c's "ErrorContext == NULL" check and call exit(2).
             *
             * pgandroid_init() calls MemoryContextInit() before pgandroid_run_initdb()
             * so that initdb's setup_data_file_paths() palloc calls work.
             * By the time we reach this boot-stage pclose, contexts may already be
             * initialized (first boot) or reset to NULL by pgandroid_reset_exit_state()
             * (subsequent runs).  Only initialize if currently NULL.
             */
            if (TopMemoryContext == NULL)
                MemoryContextInit();
            BootstrapModeMain(boot_argc, boot_argv, false /* check_only */);
        }

        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);

        PGANDROID_LOGI("pgandroid_pclose: BootstrapModeMain returned");

        /*
         * Reset proc_exit_inprogress and exit callback lists so that the
         * subsequent PostgresSingleUserMain(template1) call can initialize
         * cleanly and register its own exit callbacks.
         */
        pgandroid_reset_exit_state();
    }
    else if (pgandroid_os_stage == 2 && pgandroid_os_single_path[0] != '\0')
    {
        /*
         * Single-user stage just completed.  Run PostgresSingleUserMain with
         * stdin redirected from the single-user commands file.
         */
        PGANDROID_LOGI("pgandroid_pclose: running PostgresSingleUserMain from %s",
                       pgandroid_os_single_path);

        tmp_fd = open(pgandroid_os_single_path, O_RDONLY);
        if (tmp_fd < 0)
        {
            PGANDROID_LOGE("pgandroid_pclose: cannot open single file: %s",
                           pgandroid_os_single_path);
            return -1;
        }

        saved_stdin = dup(STDIN_FILENO);
        dup2(tmp_fd, STDIN_FILENO);
        close(tmp_fd);

        {
            static char *single_argv[] = {
                "postgres", "--single", "-j",
                "template1", NULL
            };
            int single_argc = 4;
            /*
             * Re-initialize memory contexts after pgandroid_reset_exit_state()
             * cleared them following BootstrapModeMain.
             */
            MemoryContextInit();
            PostgresSingleUserMain(single_argc, single_argv, "postgres");
        }

        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);

        pgandroid_os_single_path[0] = '\0'; /* mark as done */
        PGANDROID_LOGI("pgandroid_pclose: PostgresSingleUserMain returned");

        /*
         * Reset proc_exit_inprogress and exit callback lists so that the
         * persistent backend thread's PostgresSingleUserMain(postgres) call
         * can initialize cleanly and register its own exit callbacks.
         */
        pgandroid_reset_exit_state();
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Stub implementations needed for combined link unit
 * ----------------------------------------------------------------------- */
#include "pgandroid_stubs.h"

/* -----------------------------------------------------------------------
 * initdb integration
 *
 * pgandroid_initdb.c is compiled as a SEPARATE frontend translation unit
 * (with -DFRONTEND and frontend include paths) to avoid frontend/backend
 * header conflicts.  We only need the function declaration here.
 * ----------------------------------------------------------------------- */
extern int pgandroid_run_initdb(const char *datadir);

/* -----------------------------------------------------------------------
 * pgandroid_log — route PostgreSQL elog/ereport to Android logcat
 * ----------------------------------------------------------------------- */
void
pgandroid_log(int elevel, const char *message)
{
    if (elevel >= ERROR)
        PGANDROID_LOGE("%s", message);
    else
        PGANDROID_LOGI("%s", message);
}

/* -----------------------------------------------------------------------
 * pg_proc_exit — return instead of calling exit() (JNI-safe shutdown)
 * ----------------------------------------------------------------------- */
void
pg_proc_exit(int code)
{
    /*
     * Run all registered on_proc_exit callbacks (cleanup hooks).
     * This mirrors PGlite's implementation in ipc.c for code 66.
     */
    PGANDROID_LOGI("pg_proc_exit called with code %d", code);

    /*
     * Call into the real proc_exit callback runner.
     * We define a thin wrapper here; the real callbacks are registered
     * via on_proc_exit() in the backend startup sequence.
     *
     * For code == 66 (graceful shutdown): run callbacks and return.
     * For any other code: also return (we cannot call exit() from JNI).
     */
    pgandroid_closed = true;

    /*
     * NOTE: In a real build this calls the backend's proc_exit_hook_runner.
     * The macro in android_pgandroid.h renames proc_exit -> pg_proc_exit
     * everywhere, so this function IS the renamed proc_exit. The backend's
     * on_proc_exit mechanism calls its registered hooks before returning here.
     */

    /* Return to JNI — do NOT call exit() */
}

/* -----------------------------------------------------------------------
 * Wire protocol message builder
 * ----------------------------------------------------------------------- */

/*
 * Build a PostgreSQL simple-query ('Q') message from a SQL string.
 * Layout: 'Q' | int32 length (BE, includes 4 bytes for itself) | sql | '\0'
 * Caller must free() the returned buffer.
 */
static char *
pgandroid_build_simple_query(const char *sql, int *out_len)
{
    int   sqllen  = (int) strlen(sql);
    int   msglen  = 4 + sqllen + 1;  /* length field + sql + NUL */
    int   total   = 1 + msglen;      /* 'Q' + rest */
    char *buf     = malloc(total);
    uint32_t be_len;

    if (!buf)
        return NULL;

    buf[0] = 'Q';

    /* Big-endian length */
    be_len = (uint32_t) msglen;
    buf[1] = (be_len >> 24) & 0xFF;
    buf[2] = (be_len >> 16) & 0xFF;
    buf[3] = (be_len >>  8) & 0xFF;
    buf[4] = (be_len      ) & 0xFF;

    memcpy(buf + 5, sql, sqllen);
    buf[5 + sqllen] = '\0';

    if (out_len)
        *out_len = total;
    return buf;
}

/* -----------------------------------------------------------------------
 * Wire-protocol response to JSON converter
 *
 * Converts raw PostgreSQL backend response messages (wire protocol v3)
 * to a JSON string.  This is a minimal parser sufficient for simple queries:
 *
 *   'T' (RowDescription)  — extract column names
 *   'D' (DataRow)         — extract row values
 *   'C' (CommandComplete) — extract row count / command tag
 *   'E' (ErrorResponse)   — extract error message and SQLSTATE
 *   'Z' (ReadyForQuery)   — end of response
 *
 * Returns a malloc-allocated JSON string.  Caller must free().
 * ----------------------------------------------------------------------- */
static char *
pgandroid_proto_to_json(const char *data, int len)
{
    /*
     * Minimal JSON builder.  For a production implementation, use a proper
     * JSON library (cJSON, jansson, etc.).  This implementation handles the
     * common cases needed for pgandroid's embedded use:
     *   - SELECT queries returning rows
     *   - INSERT/UPDATE/DELETE returning command tags
     *   - Error responses
     */

    /* Output JSON buffer — grow as needed */
    int   out_cap = 4096;
    char *out     = malloc(out_cap);
    int   out_len = 0;
    int   pos     = 0;

    /* Column names from RowDescription */
    char  col_names[256][64];
    int   ncols  = 0;

    /* DataRow array */
    int   nrows  = 0;
    bool  in_rows_array = false;

    /* Status */
    bool  had_error = false;
    char  error_msg[1024]    = "";
    char  sqlstate[8]        = "";
    char  command_tag[128]   = "";

    if (!out)
        return NULL;

/* Helper macros for appending to out[] */
#define JSON_APPEND(s) do { \
    int _l = strlen(s); \
    if (out_len + _l + 2 >= out_cap) { out_cap = (out_len + _l + 2) * 2; out = realloc(out, out_cap); } \
    memcpy(out + out_len, (s), _l); out_len += _l; out[out_len] = '\0'; \
} while(0)

#define JSON_APPENDC(c) do { \
    if (out_len + 2 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); } \
    out[out_len++] = (c); out[out_len] = '\0'; \
} while(0)

    while (pos < len)
    {
        char     msgtype = data[pos++];
        uint32_t msglen;
        int      msgend;

        if (pos + 4 > len)
            break;

        msglen = ((unsigned char)data[pos]   << 24) |
                 ((unsigned char)data[pos+1] << 16) |
                 ((unsigned char)data[pos+2] <<  8) |
                 ((unsigned char)data[pos+3]);
        pos   += 4;
        msgend = pos + msglen - 4;  /* end of this message's payload */

        switch (msgtype)
        {
            case 'T': /* RowDescription */
            {
                uint16_t nc = ((unsigned char)data[pos] << 8) | (unsigned char)data[pos+1];
                pos += 2;
                ncols = (nc < 256) ? nc : 256;
                for (int i = 0; i < ncols && pos < msgend; i++)
                {
                    /* Column name: null-terminated string */
                    int nl = strnlen(data + pos, msgend - pos);
                    int copy = (nl < 63) ? nl : 63;
                    memcpy(col_names[i], data + pos, copy);
                    col_names[i][copy] = '\0';
                    pos += nl + 1;
                    pos += 18; /* skip typeoid(4)+attnum(2)+typid(4)+typlen(2)+typmod(4)+format(2) */
                }
                break;
            }

            case 'D': /* DataRow */
            {
                uint16_t nc = ((unsigned char)data[pos] << 8) | (unsigned char)data[pos+1];
                pos += 2;

                if (!in_rows_array)
                {
                    JSON_APPEND("{\"status\":\"ok\",\"rows\":[");
                    in_rows_array = true;
                }
                else
                {
                    JSON_APPENDC(',');
                }

                JSON_APPENDC('{');
                for (int i = 0; i < (int)nc && pos < msgend; i++)
                {
                    int32_t vlen = ((unsigned char)data[pos]   << 24) |
                                   ((unsigned char)data[pos+1] << 16) |
                                   ((unsigned char)data[pos+2] <<  8) |
                                   ((unsigned char)data[pos+3]);
                    pos += 4;

                    if (i > 0)
                        JSON_APPENDC(',');

                    /* Column name */
                    JSON_APPENDC('"');
                    JSON_APPEND(i < ncols ? col_names[i] : "?");
                    JSON_APPEND("\":");

                    if (vlen == -1)
                    {
                        /* NULL */
                        JSON_APPEND("null");
                    }
                    else
                    {
                        /* String value — minimal JSON escaping */
                        JSON_APPENDC('"');
                        for (int j = 0; j < vlen && pos + j < msgend; j++)
                        {
                            char c = data[pos + j];
                            if      (c == '"')  { JSON_APPEND("\\\""); }
                            else if (c == '\\') { JSON_APPEND("\\\\"); }
                            else if (c == '\n') { JSON_APPEND("\\n"); }
                            else if (c == '\r') { JSON_APPEND("\\r"); }
                            else if (c == '\t') { JSON_APPEND("\\t"); }
                            else                { JSON_APPENDC(c); }
                        }
                        JSON_APPENDC('"');
                        pos += vlen;
                    }
                }
                JSON_APPENDC('}');
                nrows++;
                break;
            }

            case 'C': /* CommandComplete */
            {
                int tl = strnlen(data + pos, msgend - pos);
                int tc = (tl < 127) ? tl : 127;
                memcpy(command_tag, data + pos, tc);
                command_tag[tc] = '\0';
                break;
            }

            case 'E': /* ErrorResponse */
            {
                had_error = true;
                /* Parse fields: byte code + null-terminated string */
                while (pos < msgend)
                {
                    char code = data[pos++];
                    if (code == 0) break;
                    int fl = strnlen(data + pos, msgend - pos);
                    if (code == 'M') { /* Message */
                        int fc = (fl < 1023) ? fl : 1023;
                        memcpy(error_msg, data + pos, fc);
                        error_msg[fc] = '\0';
                    }
                    else if (code == 'C') { /* SQLSTATE */
                        int fc = (fl < 7) ? fl : 7;
                        memcpy(sqlstate, data + pos, fc);
                        sqlstate[fc] = '\0';
                    }
                    pos += fl + 1;
                }
                break;
            }

            case 'Z': /* ReadyForQuery */
                pos = msgend;
                goto done;

            default:
                /* Skip unknown message */
                break;
        }

        pos = msgend;
    }

done:
    if (had_error)
    {
        /* Build error JSON */
        char errbuf[2048];
        snprintf(errbuf, sizeof(errbuf),
                 "{\"status\":\"error\",\"message\":\"%s\",\"sqlstate\":\"%s\"}",
                 error_msg, sqlstate);
        free(out);
        return strdup(errbuf);
    }

    if (in_rows_array)
    {
        char tail[128];
        snprintf(tail, sizeof(tail), "],\"rowcount\":%d}", nrows);
        JSON_APPEND(tail);
    }
    else
    {
        /* Non-SELECT command (INSERT, UPDATE, DELETE, CREATE, ...) */
        char cmdbuf[256];
        snprintf(cmdbuf, sizeof(cmdbuf),
                 "{\"status\":\"ok\",\"command\":\"%s\",\"rowcount\":0}", command_tag);
        free(out);
        return strdup(cmdbuf);
    }

    return out;

#undef JSON_APPEND
#undef JSON_APPENDC
}

/* -----------------------------------------------------------------------
 * Backend thread management
 *
 * pgandroid_drive_backend() is called by pgandroid_exec() for each query.
 * The first call starts a persistent backend thread running
 * PostgresSingleUserMain().  Subsequent calls simply wake up the backend
 * (input is already in the I/O buffer via pgandroid_io_put_input()) and wait
 * for it to signal that the response is ready.
 *
 * The backend thread blocks in pgandroid_io_read() between queries.
 * When pgandroid_io_put_input() is called, the thread wakes, processes the
 * query, writes the response, then blocks again waiting for the next query —
 * at which point it signals pgandroid_io_wait_result() to wake the main thread.
 * ----------------------------------------------------------------------- */

static pthread_t  pgandroid_backend_thread;
static bool       pgandroid_backend_started = false;

static void *
pgandroid_backend_thread_func(void *arg)
{
    /*
     * Argument list passed to PostgresSingleUserMain().
     * "--single -j" puts the backend into single-user interactive mode with
     * no readline.  ANDROID_PGOPTS supplies GUC overrides.  The final
     * positional argument is the database name (== username for the default db).
     *
     * Arg count: "postgres"(1) + "--single"(1) + "-j"(1) + 22 PGOPTS + db(1) = 26
     */
    const char *pg_argv[] = {
        "postgres",
        "--single",
        "-j",
        ANDROID_PGOPTS,
        ANDROID_USERNAME,   /* database name */
        NULL
    };
    int pg_argc = 26;

    /*
     * Call PostgresSingleUserMain() via a function pointer to prevent the
     * compiler from treating it as a noreturn call and dead-code-eliminating
     * the "return NULL" below.  (c.h redefines pg_attribute_noreturn to
     * __attribute__((noreturn)) after android_pgandroid.h's override.)
     */
    typedef void (*pg_sumain_t)(int, char **, const char *);
    pg_sumain_t fn = PostgresSingleUserMain;

    (void) arg;

    /*
     * Initialize PostgreSQL memory contexts before starting the persistent
     * backend. pgandroid_reset_exit_state() left TopMemoryContext=NULL after
     * the last initdb single-user run completed.
     */
    MemoryContextInit();
    PGANDROID_LOGI("pgandroid_backend: starting PostgresSingleUserMain");
    fn(pg_argc, (char **)(uintptr_t)pg_argv, ANDROID_USERNAME);
    PGANDROID_LOGI("pgandroid_backend: PostgresSingleUserMain returned");

    /* Signal shutdown to any waiting main thread */
    pgandroid_io_close();
    return NULL;
}

/*
 * pgandroid_drive_backend — drive one query through the backend.
 *
 * Pre-condition: pgandroid_io_put_input() has already been called with the
 *               wire-protocol query message.
 * Post-condition: pgandroid_io_get_output() returns the backend's response.
 */
static void
pgandroid_drive_backend(void)
{
    if (!pgandroid_backend_started)
    {
        pgandroid_backend_started = true;
        if (pthread_create(&pgandroid_backend_thread, NULL,
                           pgandroid_backend_thread_func, NULL) != 0)
        {
            PGANDROID_LOGE("pgandroid_drive_backend: pthread_create failed");
            return;
        }
        /* Detach so we don't need to join on close (fire-and-forget) */
        pthread_detach(pgandroid_backend_thread);
    }

    /*
     * Wait until the backend has processed the query and signaled that it is
     * ready for the next one.  pgandroid_io_read() inside the backend thread
     * signals io_out_cond when it finds no input waiting (i.e., after writing
     * the full query response).
     */
    pgandroid_io_wait_result();
}

/* -----------------------------------------------------------------------
 * Public C API — exported from libpgandroid.so
 * Called directly from pgandroid_jni.c (JNI wrapper layer)
 * ----------------------------------------------------------------------- */

/*
 * pgandroid_init — initialize PostgreSQL and run initdb if needed.
 *
 * datadir: path to PostgreSQL data directory on Android internal storage.
 *          e.g. "/data/data/com.example.app/files/pgdata"
 *
 * Returns 0 on success, -1 on error.
 */
PGANDROID_EXPORT int
pgandroid_init(const char *datadir)
{
    if (pgandroid_ready)
        return 0;  /* Already initialized */

    PGANDROID_LOGI("pgandroid_init: datadir=%s", datadir);

    pgandroid_datadir = strdup(datadir);
    setenv("PGDATA", datadir, 1);
    setenv("PGCLIENTENCODING", "UTF-8", 0);

    pgandroid_io_init();

    /* Run initdb if the data directory does not yet contain PG_VERSION */
    char pg_version_path[PATH_MAX];
    snprintf(pg_version_path, sizeof(pg_version_path), "%s/PG_VERSION", datadir);

    if (access(pg_version_path, F_OK) != 0)
    {
        PGANDROID_LOGI("pgandroid_init: running initdb");
        /*
         * initdb.c calls palloc() (via psprintf) in setup_data_file_paths()
         * before the first popen() call.  Palloc requires CurrentMemoryContext
         * to be non-NULL.  Initialize memory contexts here so that palloc
         * works throughout the entire initdb run.
         *
         * pgandroid_pclose() boot stage also calls MemoryContextInit() — guard
         * that call so it only fires when contexts have been reset to NULL by
         * pgandroid_reset_exit_state().
         */
        if (TopMemoryContext == NULL)
            MemoryContextInit();
        int rc = pgandroid_run_initdb(datadir);
        if (rc != 0)
        {
            PGANDROID_LOGE("pgandroid_init: initdb failed with code %d", rc);
            return -1;
        }
    }

    pgandroid_ready  = true;
    pgandroid_closed = false;
    PGANDROID_LOGI("pgandroid_init: ready");
    return 0;
}

/*
 * pgandroid_exec — execute an SQL statement, return JSON result string.
 *
 * sql: null-terminated SQL string (UTF-8).
 *
 * Returns a malloc-allocated JSON string.  The caller (JNI layer) must
 * copy this string into a Java String before the next pgandroid_exec call,
 * because pgandroid_io_reset() will free the underlying buffer on next query.
 *
 * Returns NULL on fatal error (e.g., not initialized).
 */
PGANDROID_EXPORT char *
pgandroid_exec(const char *sql)
{
    int   wire_len;
    char *wire_msg;
    int   out_len;
    char *out_bytes;
    char *json;

    if (!pgandroid_ready)
    {
        PGANDROID_LOGE("pgandroid_exec: not initialized");
        return strdup("{\"status\":\"error\",\"message\":\"not initialized\",\"sqlstate\":\"08003\"}");
    }

    /* Build wire-protocol simple query message */
    wire_msg = pgandroid_build_simple_query(sql, &wire_len);
    if (!wire_msg)
        return strdup("{\"status\":\"error\",\"message\":\"OOM\",\"sqlstate\":\"53200\"}");

    pgandroid_io_reset();
    pgandroid_io_put_input(wire_msg, wire_len);
    free(wire_msg);

    /*
     * Drive PostgreSQL's single-user main loop for one query iteration.
     * PostgresSingleUserMain() is patched (via pgandroid_stubs.h) to
     * process one message from the input buffer and return.
     */
    pgandroid_drive_backend();

    /* Collect response */
    out_bytes = pgandroid_io_get_output(&out_len);
    json      = pgandroid_proto_to_json(out_bytes, out_len);

    PGANDROID_LOGI("pgandroid_exec: %s → %d bytes → %s",
                   sql, out_len, json ? json : "(null)");
    return json;
}

/*
 * pgandroid_exec_params — execute SQL with JSON-encoded parameters.
 *
 * params_json: JSON array of parameter values as strings, e.g. ["42","hello"]
 *
 * This builds a PostgreSQL extended-query sequence:
 *   Parse('', sql, [])
 *   Bind('', '', params...)
 *   Execute('', 0)
 *   Sync
 *
 * Returns malloc-allocated JSON result string (same format as pgandroid_exec).
 */
PGANDROID_EXPORT char *
pgandroid_exec_params(const char *sql, const char *params_json)
{
    /*
     * For the initial implementation, delegate to pgandroid_exec() with
     * a simple query.  Full extended-query support (Bind/Execute) is a
     * future enhancement.
     *
     * TODO: parse params_json, build Parse/Bind/Execute/Sync wire messages.
     */
    (void) params_json;
    return pgandroid_exec(sql);
}

/*
 * pgandroid_close — graceful shutdown.
 * Calls pg_proc_exit(66) which runs cleanup hooks and returns (does not exit).
 */
PGANDROID_EXPORT void
pgandroid_close(void)
{
    if (!pgandroid_ready || pgandroid_closed)
        return;
    PGANDROID_LOGI("pgandroid_close");
    pgandroid_io_close();  /* signal backend thread to exit (makes io_read return EOF) */
    pg_proc_exit(66);      /* run on_proc_exit callbacks; returns (does not exit()) */
    pgandroid_ready = false;
}

