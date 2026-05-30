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
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/catcache.h"
#include "access/xlog.h"
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

/*
 * When true, PostgresMain() sets whereToSendOutput = DestRemote so
 * I/O goes through the wire protocol (pgandroid_io_read/write) instead
 * of stdin/stdout.  Only the persistent backend thread sets this; the
 * initdb --single phases keep DestDebug to read SQL from the tmpfile.
 */
bool pgandroid_wire_mode = false;

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

    /* Clear PGPROC so InitProcess() can allocate a fresh slot */
    MyProc = NULL;

    /*
     * Close all kernel file descriptors tracked by the VFD table.
     * Must happen BEFORE we null memory contexts since elog needs them.
     * The VFD table itself becomes stale after the memory context reset,
     * but InitFileAccess() in the next phase creates a fresh one.
     */
    closeAllVfds();

    /*
     * Discard smgr hash table without trying to close files (already
     * closed by closeAllVfds above).  Without this, the next phase's
     * cache invalidation would try to mdclose stale File handles.
     */
    smgr_android_reset();

    /*
     * Reset cache globals so RelationCacheInitialize/InitCatCache
     * re-create them from scratch instead of using stale pointers.
     *  - CacheMemoryContext: must be NULL so CreateCacheMemoryContext() runs
     *  - criticalRelcachesBuilt: must be false so nailed-in relations get loaded
     *  - criticalSharedRelcachesBuilt: same for shared catalogs
     */
    CacheMemoryContext = NULL;
    criticalRelcachesBuilt = false;
    criticalSharedRelcachesBuilt = false;

    /*
     * Reset XLOG process-local state: LocalXLogInsertAllowed back to -1
     * (unknown) and LocalRecoveryInProgress back to true (recheck needed).
     * Without this, ShutdownXLOG's "never write WAL again" flag persists
     * into the next phase and causes PANIC in CreateCheckPoint.
     */
    xlog_android_reset();

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
    /*
     * Reset state from any previous phase (bootstrap → single-user).
     * This nulls TopMemoryContext, clears exit callbacks, etc.
     * Must happen HERE (not in pclose_v2) because initdb code continues
     * running between pclose and the next popen/pclose cycle and needs
     * working memory contexts.
     */
    pgandroid_reset_state();

    /* Set process ID (used by various PG subsystems) */
    MyProcPid = (int) getpid();

    /* Initialize fresh memory contexts */
    MemoryContextInit();

    /* Reset progname for error messages */
    progname = "postgres";

    /* Reset getopt state for argument parsing */
    optind = 1;

    if (argc > 1 && strcmp(argv[1], "--boot") == 0)
    {
        PGANDROID_LOGI("pgandroid_postgres: calling BootstrapModeMain");
        BootstrapModeMain(argc, argv, false);
        {
            const char msg[] = "PGANDROID: BootstrapModeMain returned to pgandroid_postgres\n";
            write(STDERR_FILENO, msg, sizeof(msg)-1);
        }
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
                /*
                 * With DestRemote, the backend sends a ReadyForQuery BEFORE
                 * the first query response.  Skip it if we haven't seen any
                 * query data yet; exit only when it marks end-of-response.
                 */
                if (in_rows_array || command_tag[0] || had_error)
                    goto done;
                break;

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
    /*
     * Backend-specific options: like ANDROID_PGOPTS but WITHOUT
     * search_path=pg_catalog (we need public schema for DDL) and
     * WITHOUT exit_on_error=true (errors should not kill the backend).
     */
    const char *pg_argv[] = {
        "postgres",
        "--single",
        "-j",
        "-c", "log_checkpoints=false",
        "-c", "ignore_invalid_pages=on",
        "-c", "temp_buffers=8MB",
        "-c", "work_mem=4MB",
        "-c", "fsync=on",
        "-c", "synchronous_commit=on",
        "-c", "wal_buffers=4MB",
        "-c", "min_wal_size=80MB",
        "-c", "shared_buffers=128MB",
        ANDROID_USERNAME,
        NULL
    };
    int pg_argc = 22;

    (void) arg;

    /*
     * Reset state from the last initdb phase (MyProc, VFDs, smgr, etc.)
     * then create fresh memory contexts.
     */
    pgandroid_reset_state();
    MemoryContextInit();

    MyProcPid = (int) getpid();
    progname = "postgres";
    optind = 1;

    /* Enable wire protocol mode so PostgresMain uses DestRemote */
    pgandroid_wire_mode = true;

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

    /*
     * Set my_exec_path so PG can compute share/lib paths on the device.
     * make_relative_path() derives <parent>/share/postgresql from
     * <parent>/bin/postgres.  The binary doesn't need to exist —
     * PG just uses the directory structure for relative path math.
     *
     * datadir = .../pgandroid/<dbName>
     * parent  = .../pgandroid
     * my_exec_path = .../pgandroid/bin/postgres
     *  → share resolves to .../pgandroid/share/postgresql  (where we extract assets)
     *  → pkglib resolves to .../pgandroid/lib              (not used; dlopen intercepted)
     */
    {
        char parent[PATH_MAX];
        strlcpy(parent, datadir, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent)
            *slash = '\0';
        snprintf(my_exec_path, MAXPGPATH, "%s/bin/postgres", parent);
        PGANDROID_LOGI("pgandroid_init: my_exec_path=%s", my_exec_path);
    }

    /*
     * Initialize memory contexts early so ErrorContext exists.
     * Without this, any accidental backend elog() call (e.g. from
     * combined-link symbol resolution) hits exit(2) with no message.
     */
    if (TopMemoryContext == NULL)
        MemoryContextInit();

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
 *
 * Substitutes $1, $2, ... placeholders with properly escaped literal
 * values from params_json (a JSON array: ["val1", null, "val2"]).
 *
 * String values are wrapped in single quotes with internal quotes doubled
 * (standard PostgreSQL escaping).  JSON null becomes SQL NULL.
 *
 * Note: this performs text-based substitution rather than using the
 * extended query protocol (Parse/Bind/Execute/Sync).  It handles the
 * common case where $N placeholders appear outside string literals.
 */

/*
 * parse_json_string_value — parse one JSON string starting at *pos.
 * Handles escape sequences (\", \\, \n, \r, \t, \uXXXX).
 * Advances *pos past the closing quote.
 * Returns malloc'd string, or NULL on error.
 */
static char *
parse_json_string_value(const char *json, int *pos)
{
    int    cap = 128;
    int    len = 0;
    char  *buf = malloc(cap);
    int    p   = *pos;

    if (!buf)
        return NULL;

    /* Expect opening quote already consumed; p points inside string */
    while (json[p] && json[p] != '"')
    {
        if (len + 8 >= cap)
        {
            cap *= 2;
            buf  = realloc(buf, cap);
            if (!buf)
                return NULL;
        }

        if (json[p] == '\\' && json[p + 1])
        {
            p++;
            switch (json[p])
            {
                case '"':  buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case '/':  buf[len++] = '/';  break;
                case 'u':
                    /* \uXXXX — simplified: just pass through as-is for BMP */
                    buf[len++] = '\\';
                    buf[len++] = 'u';
                    break;
                default:
                    buf[len++] = json[p];
                    break;
            }
            p++;
        }
        else
        {
            buf[len++] = json[p++];
        }
    }

    if (json[p] == '"')
        p++;  /* skip closing quote */

    buf[len] = '\0';
    *pos = p;
    return buf;
}

/*
 * parse_json_params — parse a JSON array of strings/nulls.
 * Input:  ["val1", null, "val2"]  or  []
 * Output: array of char* (malloc'd strings, NULL for json null).
 *         *out_count set to number of elements.
 * Caller must free each non-NULL element and the array itself.
 */
static char **
parse_json_params(const char *json, int *out_count)
{
    int     cap    = 8;
    int     count  = 0;
    char  **params = calloc(cap, sizeof(char *));
    int     p      = 0;

    *out_count = 0;
    if (!params || !json)
        return params;

    /* Skip whitespace and opening bracket */
    while (json[p] && json[p] != '[')
        p++;
    if (json[p] == '[')
        p++;

    while (json[p])
    {
        /* Skip whitespace and commas */
        while (json[p] == ' ' || json[p] == ',' || json[p] == '\t' ||
               json[p] == '\n' || json[p] == '\r')
            p++;

        if (json[p] == ']' || json[p] == '\0')
            break;

        /* Grow array if needed */
        if (count >= cap)
        {
            cap *= 2;
            params = realloc(params, cap * sizeof(char *));
            if (!params)
                return NULL;
        }

        if (json[p] == '"')
        {
            /* String value */
            p++;  /* skip opening quote */
            params[count] = parse_json_string_value(json, &p);
            count++;
        }
        else if (strncmp(json + p, "null", 4) == 0)
        {
            /* JSON null → SQL NULL */
            params[count] = NULL;
            count++;
            p += 4;
        }
        else
        {
            /* Skip unexpected token */
            p++;
        }
    }

    *out_count = count;
    return params;
}

/*
 * expand_sql_params — replace $1, $2, ... in SQL with escaped literals.
 * Returns malloc'd expanded SQL string.
 */
static char *
expand_sql_params(const char *sql, char **params, int nparams)
{
    int    sql_len = (int) strlen(sql);
    int    cap     = sql_len * 2 + 256;
    int    len     = 0;
    char  *out     = malloc(cap);
    int    i       = 0;

    if (!out)
        return NULL;

    while (sql[i])
    {
        /* Check for $N placeholder */
        if (sql[i] == '$' && sql[i + 1] >= '1' && sql[i + 1] <= '9')
        {
            /* Parse the parameter number */
            int param_num = 0;
            int j = i + 1;
            while (sql[j] >= '0' && sql[j] <= '9')
            {
                param_num = param_num * 10 + (sql[j] - '0');
                j++;
            }

            if (param_num >= 1 && param_num <= nparams)
            {
                int idx = param_num - 1;  /* 0-based */
                if (params[idx] == NULL)
                {
                    /* NULL literal */
                    const char *null_str = "NULL";
                    int nl = 4;
                    if (len + nl + 1 >= cap)
                    {
                        cap = (len + nl + 8) * 2;
                        out = realloc(out, cap);
                        if (!out) return NULL;
                    }
                    memcpy(out + len, null_str, nl);
                    len += nl;
                }
                else
                {
                    /* Quoted literal with escaped single quotes */
                    const char *val = params[idx];
                    int vlen = (int) strlen(val);
                    /* Worst case: every char is a quote (doubled) + 2 surrounding quotes */
                    if (len + vlen * 2 + 4 >= cap)
                    {
                        cap = (len + vlen * 2 + 8) * 2;
                        out = realloc(out, cap);
                        if (!out) return NULL;
                    }
                    out[len++] = '\'';
                    for (int k = 0; k < vlen; k++)
                    {
                        if (val[k] == '\'')
                            out[len++] = '\'';  /* double the quote */
                        out[len++] = val[k];
                    }
                    out[len++] = '\'';
                }
                i = j;  /* advance past $N */
                continue;
            }
        }

        /* Regular character — copy through */
        if (len + 2 >= cap)
        {
            cap *= 2;
            out = realloc(out, cap);
            if (!out) return NULL;
        }
        out[len++] = sql[i++];
    }

    out[len] = '\0';
    return out;
}

PGANDROID_EXPORT char *
pgandroid_exec_params(const char *sql, const char *params_json)
{
    char  **params = NULL;
    int     nparams = 0;
    char   *expanded;
    char   *result;

    /* No params or empty array — execute directly */
    if (!params_json || strcmp(params_json, "[]") == 0)
        return pgandroid_exec(sql);

    params = parse_json_params(params_json, &nparams);
    if (!params || nparams == 0)
    {
        free(params);
        return pgandroid_exec(sql);
    }

    expanded = expand_sql_params(sql, params, nparams);
    if (!expanded)
    {
        for (int i = 0; i < nparams; i++)
            free(params[i]);
        free(params);
        return pgandroid_exec(sql);
    }

    PGANDROID_LOGI("pgandroid_exec_params: expanded SQL: %.200s", expanded);
    result = pgandroid_exec(expanded);

    free(expanded);
    for (int i = 0; i < nparams; i++)
        free(params[i]);
    free(params);

    return result;
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
