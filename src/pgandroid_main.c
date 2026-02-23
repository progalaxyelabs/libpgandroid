/*
 * pgandroid/pgandroid_main.c  (v2 — direct call approach)
 *
 * Combined main translation unit for pgandroid.
 *
 * v2 key changes from v1:
 *  - NO popen/pclose interception (no pgandroid_os.h)
 *  - NO proc_exit redefinition.  Instead, #ifdef guards at call sites in
 *    bootstrap.c and postgres.c cause BootstrapModeMain and PostgresSingleUserMain
 *    to return normally instead of calling proc_exit().
 *  - pgandroid_postgres() wraps the entry-point dispatch (mimics main.c).
 *  - pgandroid_reset_state() clears state between phases.
 *
 * Build flags required:
 *   -D__ANDROID_PGANDROID__
 *   -DPGANDROID_MAIN
 *   -include android_pgandroid.h
 *
 * Entry points (exported from libpgandroid.so):
 *   pgandroid_init(const char *datadir)
 *   pgandroid_exec(const char *sql)
 *   pgandroid_exec_params(const char *sql, const char *params_json)
 *   pgandroid_close()
 */

/* -----------------------------------------------------------------------
 * PostgreSQL backend headers
 * ----------------------------------------------------------------------- */
#include "postgres.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "bootstrap/bootstrap.h"
#include "storage/ipc.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "postmaster/postmaster.h"   /* progname */

/* Android NDK logging */
#include <android/log.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define PGANDROID_TAG "pgandroid"
#define PGANDROID_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  PGANDROID_TAG, __VA_ARGS__)
#define PGANDROID_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PGANDROID_TAG, __VA_ARGS__)

/* -----------------------------------------------------------------------
 * Stub implementations needed for combined link unit
 * ----------------------------------------------------------------------- */
#include "pgandroid_stubs.h"

/* -----------------------------------------------------------------------
 * initdb integration — compiled as separate frontend TU
 * ----------------------------------------------------------------------- */
extern int pgandroid_run_initdb(const char *datadir);

/* -----------------------------------------------------------------------
 * Global state
 * ----------------------------------------------------------------------- */

static const char *pgandroid_datadir  = NULL;
static bool        pgandroid_ready    = false;
static bool        pgandroid_closed   = false;

/* -----------------------------------------------------------------------
 * pgandroid_reset_state — reset PostgreSQL state between initdb phases.
 *
 * After BootstrapModeMain or PostgresSingleUserMain returns (via #ifdef
 * guard instead of proc_exit), various global state is stale:
 *  - proc_exit_inprogress may be true (if cleanup() triggered it)
 *  - on_proc_exit callback lists contain stale entries
 *  - TopMemoryContext and children are for the old phase
 *
 * We reset the minimum necessary for the next phase to reinitialize.
 * Leaked memory is acceptable for one-time initdb.
 * ----------------------------------------------------------------------- */
void
pgandroid_reset_state(void)
{
    proc_exit_inprogress  = false;
    shmem_exit_inprogress = false;

    /* Clear exit callback lists so the next phase can register fresh */
    on_exit_reset();

    /* Reset memory context pointers so MemoryContextInit() creates fresh ones */
    TopMemoryContext    = NULL;
    CurrentMemoryContext = NULL;
    ErrorContext        = NULL;

    PGANDROID_LOGI("pgandroid_reset_state: state reset for next phase");
}

/* -----------------------------------------------------------------------
 * pgandroid_postgres — wrapper that mimics main.c dispatch.
 *
 * Calls BootstrapModeMain or PostgresSingleUserMain as if we were a
 * fresh "postgres" process.  stdin must be redirected to the input file
 * BEFORE calling this.
 *
 * In v2, these functions RETURN instead of calling proc_exit() because
 * of #ifdef guards at the call sites.
 * ----------------------------------------------------------------------- */
void
pgandroid_postgres(int argc, char *argv[])
{
    /* Set process ID (used by various PG subsystems) */
    MyProcPid = (int) getpid();

    /* Initialize memory contexts if not already done */
    if (TopMemoryContext == NULL)
        MemoryContextInit();

    /* Reset progname for error messages */
    progname = "postgres";

    /* Reset getopt state for argument parsing */
    optind = 1;

    if (argc > 1 && strcmp(argv[1], "--boot") == 0)
    {
        PGANDROID_LOGI("pgandroid_postgres: calling BootstrapModeMain");
        BootstrapModeMain(argc, argv, false);
        PGANDROID_LOGI("pgandroid_postgres: BootstrapModeMain returned");
    }
    else if (argc > 1 && strcmp(argv[1], "--single") == 0)
    {
        PGANDROID_LOGI("pgandroid_postgres: calling PostgresSingleUserMain");
        PostgresSingleUserMain(argc, argv, ANDROID_USERNAME);
        PGANDROID_LOGI("pgandroid_postgres: PostgresSingleUserMain returned");
    }
    else
    {
        PGANDROID_LOGE("pgandroid_postgres: unknown mode, argv[1]=%s",
                       argc > 1 ? argv[1] : "(none)");
    }
}

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
 * to a JSON string.  Minimal parser for simple queries:
 *   'T' (RowDescription), 'D' (DataRow), 'C' (CommandComplete),
 *   'E' (ErrorResponse), 'Z' (ReadyForQuery)
 *
 * Returns a malloc-allocated JSON string.  Caller must free().
 * ----------------------------------------------------------------------- */
static char *
pgandroid_proto_to_json(const char *data, int len)
{
    int   out_cap = 4096;
    char *out     = malloc(out_cap);
    int   out_len = 0;
    int   pos     = 0;

    char  col_names[256][64];
    int   ncols  = 0;
    int   nrows  = 0;
    bool  in_rows_array = false;
    bool  had_error = false;
    char  error_msg[1024]    = "";
    char  sqlstate[8]        = "";
    char  command_tag[128]   = "";

    if (!out)
        return NULL;

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
        msgend = pos + msglen - 4;

        switch (msgtype)
        {
            case 'T': /* RowDescription */
            {
                uint16_t nc = ((unsigned char)data[pos] << 8) | (unsigned char)data[pos+1];
                pos += 2;
                ncols = (nc < 256) ? nc : 256;
                for (int i = 0; i < ncols && pos < msgend; i++)
                {
                    int nl = strnlen(data + pos, msgend - pos);
                    int copy = (nl < 63) ? nl : 63;
                    memcpy(col_names[i], data + pos, copy);
                    col_names[i][copy] = '\0';
                    pos += nl + 1;
                    pos += 18; /* skip typeoid+attnum+typid+typlen+typmod+format */
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
                    JSON_APPENDC(',');

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

                    JSON_APPENDC('"');
                    JSON_APPEND(i < ncols ? col_names[i] : "?");
                    JSON_APPEND("\":");

                    if (vlen == -1)
                    {
                        JSON_APPEND("null");
                    }
                    else
                    {
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
                while (pos < msgend)
                {
                    char code = data[pos++];
                    if (code == 0) break;
                    int fl = strnlen(data + pos, msgend - pos);
                    if (code == 'M') {
                        int fc = (fl < 1023) ? fl : 1023;
                        memcpy(error_msg, data + pos, fc);
                        error_msg[fc] = '\0';
                    }
                    else if (code == 'C') {
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
                break;
        }

        pos = msgend;
    }

done:
    if (had_error)
    {
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
 * The first pgandroid_exec() call starts a persistent backend thread
 * running PostgresSingleUserMain().  Subsequent calls wake the backend
 * via pgandroid_io_put_input() and wait for the response.
 * ----------------------------------------------------------------------- */

static pthread_t  pgandroid_backend_thread;
static bool       pgandroid_backend_started = false;

static void *
pgandroid_backend_thread_func(void *arg)
{
    const char *pg_argv[] = {
        "postgres",
        "--single",
        "-j",
        ANDROID_PGOPTS,
        ANDROID_USERNAME,
        NULL
    };
    int pg_argc = 26;

    (void) arg;

    /*
     * Initialize memory contexts after pgandroid_reset_state() cleared them
     * following the last initdb single-user run.
     */
    if (TopMemoryContext == NULL)
        MemoryContextInit();

    MyProcPid = (int) getpid();
    progname = "postgres";
    optind = 1;

    PGANDROID_LOGI("pgandroid_backend: starting PostgresSingleUserMain");
    PostgresSingleUserMain(pg_argc, (char **)(uintptr_t)pg_argv, ANDROID_USERNAME);
    PGANDROID_LOGI("pgandroid_backend: PostgresSingleUserMain returned");

    /* Signal shutdown to any waiting main thread */
    pgandroid_io_close();
    return NULL;
}

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
        pthread_detach(pgandroid_backend_thread);
    }

    pgandroid_io_wait_result();
}

/* -----------------------------------------------------------------------
 * Public C API — exported from libpgandroid.so
 * ----------------------------------------------------------------------- */

/*
 * pgandroid_init — initialize PostgreSQL and run initdb if needed.
 * Returns 0 on success, -1 on error.
 */
PGANDROID_EXPORT int
pgandroid_init(const char *datadir)
{
    if (pgandroid_ready)
        return 0;

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
 * Returns a malloc-allocated JSON string.  Caller must free().
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

    wire_msg = pgandroid_build_simple_query(sql, &wire_len);
    if (!wire_msg)
        return strdup("{\"status\":\"error\",\"message\":\"OOM\",\"sqlstate\":\"53200\"}");

    pgandroid_io_reset();
    pgandroid_io_put_input(wire_msg, wire_len);
    free(wire_msg);

    pgandroid_drive_backend();

    out_bytes = pgandroid_io_get_output(&out_len);
    json      = pgandroid_proto_to_json(out_bytes, out_len);

    PGANDROID_LOGI("pgandroid_exec: %s -> %d bytes -> %s",
                   sql, out_len, json ? json : "(null)");
    return json;
}

/*
 * pgandroid_exec_params — execute SQL with JSON-encoded parameters.
 * TODO: implement extended query protocol (Parse/Bind/Execute/Sync).
 */
PGANDROID_EXPORT char *
pgandroid_exec_params(const char *sql, const char *params_json)
{
    (void) params_json;
    return pgandroid_exec(sql);
}

/*
 * pgandroid_close — graceful shutdown.
 */
PGANDROID_EXPORT void
pgandroid_close(void)
{
    if (!pgandroid_ready || pgandroid_closed)
        return;
    PGANDROID_LOGI("pgandroid_close");
    pgandroid_io_close();
    pgandroid_closed = true;
    pgandroid_ready  = false;
}
