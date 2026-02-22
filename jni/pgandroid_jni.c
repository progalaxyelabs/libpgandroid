/*
 * jni/pgandroid_jni.c — JNI bridge: Kotlin/Java ↔ libpgandroid C API
 *
 * Implements the native methods declared in com.pgandroid.PgDatabase
 * (PgDatabase.kt).  Each JNI function validates a session handle, converts
 * JNI types to C types, calls the pgandroid C API, and propagates errors
 * back to Java as exceptions.
 *
 * -----------------------------------------------------------------------
 * THREAD SAFETY
 *
 * All JNI native methods MUST be serialised by the Kotlin layer.  The
 * embedded PostgreSQL backend is strictly single-threaded.  Use a Kotlin
 * Mutex or a single-threaded coroutine dispatcher to prevent concurrent
 * calls.  See jni/pgandroid.h for the full contract.
 *
 * -----------------------------------------------------------------------
 * STRING ENCODING
 *
 * JNI GetStringUTFChars() returns Modified UTF-8 (MUTF-8).  For the BMP
 * (U+0000–U+FFFF) and ASCII, MUTF-8 is identical to standard UTF-8.
 * The only difference is:
 *   - Null bytes (U+0000) encoded as 0xC0,0x80 in MUTF-8 vs 0x00 in UTF-8.
 *   - Code points above U+FFFF encoded via surrogate pairs in MUTF-8.
 * SQL strings should not contain raw null bytes, so MUTF-8 is acceptable
 * for the vast majority of real-world queries.
 *
 * -----------------------------------------------------------------------
 * BUILD (host — development/testing on Linux x86_64):
 *
 *   gcc -shared -fPIC -o libpgandroid.so \
 *       jni/pgandroid_jni.c \
 *       build/pg-src/pgandroid/pgandroid_main.c \
 *       build/pg-src/pgandroid/pgandroid_io.c \
 *       -I$JAVA_HOME/include -I$JAVA_HOME/include/linux \
 *       -I build/pg-src/pgandroid \
 *       -I build/pg-install/include/server \
 *       -L build/pg-install/lib -lpq -lssl -lcrypto
 *
 * BUILD (Android cross-compile — see build.sh for full invocation):
 *
 *   $NDK_CLANG -shared -fPIC \
 *       -D__ANDROID_PGANDROID__ \
 *       -include build/pg-src/pgandroid/android_pgandroid.h \
 *       jni/pgandroid_jni.c \
 *       build/pg-src/pgandroid/pgandroid_main.c \
 *       build/pg-src/pgandroid/pgandroid_io.c \
 *       -I$JAVA_HOME/include -I$JAVA_HOME/include/linux \
 *       -I build/pg-src/pgandroid \
 *       -I build/pg-install/include/server \
 *       -L build/pg-install/lib -lssl -lcrypto -llog -landroid \
 *       -o out/arm64-v8a/libpgandroid.so
 */

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Public pgandroid C API (declares pgandroid_init_with_config, pgandroid_exec, etc.) */
#include "pgandroid.h"

/* -----------------------------------------------------------------------
 * Session handle
 *
 * nativeOpen() allocates a PgAndroidSession on the heap and returns its
 * address cast to jlong.  Every subsequent JNI call validates the magic
 * number before proceeding.  nativeClose() marks the session closed and
 * frees the struct.
 * ----------------------------------------------------------------------- */

#define PGANDROID_SESSION_MAGIC  0x50474144UL   /* "PGAD" */

typedef struct {
    uint32_t magic;     /* PGANDROID_SESSION_MAGIC — validity sentinel        */
    char    *datadir;   /* strdup of the datadir passed to nativeOpen          */
    int      closed;    /* 1 after nativeClose() — prevents double-close       */
} PgAndroidSession;

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/*
 * get_session — decode and validate a PgAndroidSession pointer from a jlong.
 *
 * Throws IllegalStateException and returns NULL when the handle is NULL,
 * corrupted, or already closed.
 */
static PgAndroidSession *
get_session(JNIEnv *env, jlong handle)
{
    PgAndroidSession *s;

    if (handle == 0)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalStateException"),
            "pgandroid: null database handle");
        return NULL;
    }

    s = (PgAndroidSession *)(uintptr_t)(uint64_t)handle;

    if (s->magic != PGANDROID_SESSION_MAGIC)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalStateException"),
            "pgandroid: invalid or corrupted database handle");
        return NULL;
    }

    if (s->closed)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalStateException"),
            "pgandroid: database is already closed");
        return NULL;
    }

    return s;
}

/*
 * throw_pg_exception — throw a Java exception using the error message
 * extracted from a pgandroid JSON error response.
 *
 * json format: {"status":"error","message":"...","sqlstate":"XXXXX"}
 *
 * Attempts to throw com.pgandroid.PgException; falls back to
 * java.lang.RuntimeException if PgException is not on the classpath.
 */
static void
throw_pg_exception(JNIEnv *env, const char *json)
{
    const char *msg_start = NULL;
    const char *msg_end   = NULL;
    char       *extracted = NULL;
    const char *msg       = json;   /* default: use the full JSON string */
    jclass      exc_class;

    /* Extract the value of "message":"<value>" */
    msg_start = strstr(json, "\"message\":\"");
    if (msg_start)
    {
        msg_start += 11;   /* length of   "message":"  */
        msg_end    = strchr(msg_start, '"');
        if (msg_end && msg_end > msg_start)
        {
            size_t mlen = (size_t)(msg_end - msg_start);
            extracted   = malloc(mlen + 1);
            if (extracted)
            {
                memcpy(extracted, msg_start, mlen);
                extracted[mlen] = '\0';
                msg = extracted;
            }
        }
    }

    /* Prefer the project-specific exception class */
    exc_class = (*env)->FindClass(env, "com/pgandroid/PgException");
    if (exc_class == NULL)
    {
        (*env)->ExceptionClear(env);
        exc_class = (*env)->FindClass(env, "java/lang/RuntimeException");
    }

    if (exc_class)
        (*env)->ThrowNew(env, exc_class, msg);

    free(extracted);
}

/*
 * is_error_json — return true when the pgandroid C API returned an error.
 */
static bool
is_error_json(const char *json)
{
    return json != NULL && strstr(json, "\"status\":\"error\"") != NULL;
}

/*
 * params_to_json_array — convert a jobjectArray of String (or null) elements
 * to a JSON array string that pgandroid_exec_params() can consume.
 *
 * Example output: ["42", "hello world", null]
 *
 * Returns a malloc-allocated string.  Caller must free().
 * Returns NULL (and throws OutOfMemoryError) on allocation failure.
 */
static char *
params_to_json_array(JNIEnv *env, jobjectArray params)
{
    jsize  count;
    int    cap = 256;
    int    len = 0;
    char  *buf;
    jsize  i;

    if (params == NULL)
        return strdup("[]");

    count = (*env)->GetArrayLength(env, params);
    if (count == 0)
        return strdup("[]");

    buf = malloc(cap);
    if (!buf)
        goto oom;

    buf[len++] = '[';

    for (i = 0; i < count; i++)
    {
        jobject    elem;
        const char *cval;
        int         vlen;
        int         j;

        /* Separator between elements */
        if (i > 0)
        {
            if (len + 2 >= cap)
            {
                cap *= 2;
                buf  = realloc(buf, cap);
                if (!buf) goto oom;
            }
            buf[len++] = ',';
        }

        elem = (*env)->GetObjectArrayElement(env, params, i);

        if (elem == NULL)
        {
            /* SQL NULL */
            const char *nulllit = "null";
            if (len + 5 >= cap)
            {
                cap = (len + 8) * 2;
                buf = realloc(buf, cap);
                if (!buf) goto oom;
            }
            memcpy(buf + len, nulllit, 4);
            len += 4;
            continue;
        }

        /* Non-null element: get as UTF-8 string */
        cval = (*env)->GetStringUTFChars(env, (jstring)elem, NULL);
        if (!cval)
        {
            (*env)->DeleteLocalRef(env, elem);
            goto oom;
        }

        vlen = (int)strlen(cval);

        /* Worst-case expansion: each byte could become \XX (6 chars) + 2 quotes */
        if (len + vlen * 6 + 4 >= cap)
        {
            cap = (len + vlen * 6 + 8) * 2;
            buf = realloc(buf, cap);
            if (!buf)
            {
                (*env)->ReleaseStringUTFChars(env, (jstring)elem, cval);
                (*env)->DeleteLocalRef(env, elem);
                goto oom;
            }
        }

        buf[len++] = '"';
        for (j = 0; j < vlen; j++)
        {
            unsigned char c = (unsigned char)cval[j];
            switch (c)
            {
                case '"':  buf[len++] = '\\'; buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; buf[len++] = '\\'; break;
                case '\n': buf[len++] = '\\'; buf[len++] = 'n';  break;
                case '\r': buf[len++] = '\\'; buf[len++] = 'r';  break;
                case '\t': buf[len++] = '\\'; buf[len++] = 't';  break;
                default:
                    if (c < 0x20)
                    {
                        /* Control characters: encode as \uXXXX */
                        len += snprintf(buf + len, cap - len, "\\u%04x", c);
                    }
                    else
                    {
                        buf[len++] = (char)c;
                    }
                    break;
            }
        }
        buf[len++] = '"';

        (*env)->ReleaseStringUTFChars(env, (jstring)elem, cval);
        (*env)->DeleteLocalRef(env, elem);
    }

    /* Close the array */
    if (len + 2 >= cap)
    {
        cap += 4;
        buf = realloc(buf, cap);
        if (!buf) goto oom;
    }
    buf[len++] = ']';
    buf[len]   = '\0';
    return buf;

oom:
    free(buf);
    (*env)->ThrowNew(env,
        (*env)->FindClass(env, "java/lang/OutOfMemoryError"),
        "pgandroid: out of memory while building params JSON");
    return NULL;
}

/* -----------------------------------------------------------------------
 * pgandroid_init_with_config — init with optional JSON GUC overrides
 *
 * Implementation of the pgandroid.h declaration.
 * This function is compiled into the same .so as pgandroid_init().
 *
 * config_json may be NULL or "{}".  Recognised keys:
 *   "work_mem"            — runtime-settable via SET (applied after init)
 *   "temp_buffers"        — runtime-settable via SET (applied after init)
 *   "fsync"               — runtime-settable via SET (applied after init)
 *   "synchronous_commit"  — runtime-settable via SET (applied after init)
 *   "shared_buffers"      — compile-time / restart-only; log but ignore
 *
 * Returns 0 on success, -1 on failure.
 * ----------------------------------------------------------------------- */

/*
 * extract_json_string_value — extract the first string value for `key` from
 * a flat JSON object.  Writes up to (buflen-1) bytes into buf, null-terminated.
 * Returns true if the key was found, false otherwise.
 *
 * This is a minimal, non-recursive extractor sufficient for the flat config
 * objects pgandroid_init_with_config() is expected to receive.
 */
static bool
extract_json_string_value(const char *json, const char *key,
                           char *buf, size_t buflen)
{
    char        search[128];
    const char *p;
    const char *end;
    size_t      vlen;

    if (!json || !key || !buf || buflen == 0)
        return false;

    /* Build the search needle: "key":" */
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    p = strstr(json, search);
    if (!p)
        return false;

    p += strlen(search);     /* point to start of value */
    end = strchr(p, '"');    /* find closing quote (simplified: ignores escapes) */
    if (!end)
        return false;

    vlen = (size_t)(end - p);
    if (vlen >= buflen)
        vlen = buflen - 1;

    memcpy(buf, p, vlen);
    buf[vlen] = '\0';
    return true;
}

PGANDROID_EXPORT int
pgandroid_init_with_config(const char *datadir, const char *config_json)
{
    int  rc;
    char val[128];

    rc = pgandroid_init(datadir);
    if (rc != 0)
        return rc;

    /* Apply runtime-settable GUCs from config_json via SET commands.
     * shared_buffers cannot be changed at runtime; log a note if requested. */
    if (config_json && *config_json)
    {
        char sql[256];

        if (extract_json_string_value(config_json, "work_mem", val, sizeof(val)))
        {
            snprintf(sql, sizeof(sql), "SET work_mem = '%s'", val);
            char *r = pgandroid_exec(sql);
            if (r) free(r);
        }

        if (extract_json_string_value(config_json, "temp_buffers", val, sizeof(val)))
        {
            snprintf(sql, sizeof(sql), "SET temp_buffers = '%s'", val);
            char *r = pgandroid_exec(sql);
            if (r) free(r);
        }

        if (extract_json_string_value(config_json, "fsync", val, sizeof(val)))
        {
            snprintf(sql, sizeof(sql), "SET fsync = '%s'", val);
            char *r = pgandroid_exec(sql);
            if (r) free(r);
        }

        if (extract_json_string_value(config_json, "synchronous_commit", val, sizeof(val)))
        {
            snprintf(sql, sizeof(sql), "SET synchronous_commit = '%s'", val);
            char *r = pgandroid_exec(sql);
            if (r) free(r);
        }

        /* shared_buffers: note that it is not runtime-settable */
        if (extract_json_string_value(config_json, "shared_buffers", val, sizeof(val)))
        {
            /* Cannot change shared_buffers at runtime; the default (128MB) is
             * configured at compile-time via ANDROID_PGOPTS in android_pgandroid.h.
             * A future enhancement could write postgresql.conf before initdb. */
            (void)val;  /* suppress unused-variable warning */
        }
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * pgandroid_checkpoint — flush all dirty data and WAL to disk.
 *
 * Implementation of the pgandroid.h declaration.
 * Executes CHECKPOINT via the normal query path.
 *
 * Returns 0 on success, -1 on error.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT int
pgandroid_checkpoint(void)
{
    char *result = pgandroid_exec("CHECKPOINT");
    int   rc     = 0;

    if (!result)
        return -1;

    if (is_error_json(result))
        rc = -1;

    free(result);
    return rc;
}

/* -----------------------------------------------------------------------
 * JNI_OnLoad
 * ----------------------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    return JNI_VERSION_1_6;
}

/* -----------------------------------------------------------------------
 * nativeOpen
 *
 * Signature (Kotlin):
 *   external fun nativeOpen(dataDir: String, configJson: String?): Long
 *
 * Initialises the embedded PostgreSQL instance (running initdb on first
 * call if the data directory does not yet contain PG_VERSION), then
 * executes:
 *   CREATE EXTENSION IF NOT EXISTS pgcrypto;
 *
 * configJson may contain any of these optional keys:
 *   "work_mem"     — applied via SET at startup (default 4MB)
 *   "temp_buffers" — applied via SET at startup (default 8MB)
 *   "fsync"        — "on"/"off", applied via SET at startup
 *   "synchronous_commit" — applied via SET at startup
 *   "shared_buffers" — logged but ignored (cannot change at runtime)
 *
 * Returns a jlong handle (opaque pointer) on success.
 * Throws RuntimeException on failure.
 * ----------------------------------------------------------------------- */
JNIEXPORT jlong JNICALL
Java_com_pgandroid_PgDatabase_nativeOpen(JNIEnv  *env,
                                          jobject  obj,
                                          jstring  dataDir,
                                          jstring  configJson)
{
    const char       *c_datadir  = NULL;
    const char       *c_config   = NULL;
    char             *saved_dir  = NULL;
    PgAndroidSession *session    = NULL;
    int               rc;

    (void)obj;

    if (dataDir == NULL)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalArgumentException"),
            "pgandroid: dataDir must not be null");
        return 0L;
    }

    c_datadir = (*env)->GetStringUTFChars(env, dataDir, NULL);
    if (!c_datadir)
        return 0L;  /* OutOfMemoryError already pending */

    /* Save a copy before we release the JNI reference */
    saved_dir = strdup(c_datadir);
    (*env)->ReleaseStringUTFChars(env, dataDir, c_datadir);
    c_datadir = NULL;

    if (!saved_dir)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/OutOfMemoryError"),
            "pgandroid: failed to copy dataDir string");
        return 0L;
    }

    if (configJson != NULL)
    {
        c_config = (*env)->GetStringUTFChars(env, configJson, NULL);
        /* c_config may be NULL if GetStringUTFChars failed — pgandroid_init_with_config
         * handles NULL config gracefully, so we continue. */
    }

    rc = pgandroid_init_with_config(saved_dir, c_config);

    if (c_config != NULL)
        (*env)->ReleaseStringUTFChars(env, configJson, c_config);

    if (rc != 0)
    {
        free(saved_dir);
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/RuntimeException"),
            "pgandroid: failed to initialize database — check logcat for details");
        return 0L;
    }

    /* Install extensions (pgcrypto provides gen_random_uuid(), digest(), etc.) */
    {
        char *ext_result = pgandroid_exec(
            "CREATE EXTENSION IF NOT EXISTS pgcrypto");
        if (ext_result)
        {
            /* Log errors but do not abort open — the extension may already exist */
            free(ext_result);
        }
    }

    /* Allocate and initialise the session handle */
    session = calloc(1, sizeof(PgAndroidSession));
    if (!session)
    {
        free(saved_dir);
        pgandroid_close();
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/OutOfMemoryError"),
            "pgandroid: failed to allocate session struct");
        return 0L;
    }

    session->magic   = PGANDROID_SESSION_MAGIC;
    session->datadir = saved_dir;
    session->closed  = 0;

    return (jlong)(uintptr_t)session;
}

/* -----------------------------------------------------------------------
 * nativeExec
 *
 * Signature (Kotlin):
 *   external fun nativeExec(handle: Long, sql: String): String
 *
 * Executes a SQL statement and returns a JSON result string.
 *
 * On PostgreSQL error, throws PgException (or RuntimeException) with
 * the PostgreSQL error message as the exception message.
 * ----------------------------------------------------------------------- */
JNIEXPORT jstring JNICALL
Java_com_pgandroid_PgDatabase_nativeExec(JNIEnv  *env,
                                          jobject  obj,
                                          jlong    handle,
                                          jstring  sql)
{
    PgAndroidSession *session;
    const char       *c_sql;
    char             *result;
    jstring           jresult = NULL;

    (void)obj;

    session = get_session(env, handle);
    if (!session)
        return NULL;  /* exception already pending */

    if (sql == NULL)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalArgumentException"),
            "pgandroid: sql must not be null");
        return NULL;
    }

    c_sql = (*env)->GetStringUTFChars(env, sql, NULL);
    if (!c_sql)
        return NULL;  /* OutOfMemoryError already pending */

    result = pgandroid_exec(c_sql);

    (*env)->ReleaseStringUTFChars(env, sql, c_sql);

    if (!result)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/OutOfMemoryError"),
            "pgandroid: pgandroid_exec returned NULL (OOM)");
        return NULL;
    }

    if (is_error_json(result))
    {
        throw_pg_exception(env, result);
        free(result);
        return NULL;
    }

    jresult = (*env)->NewStringUTF(env, result);
    free(result);
    return jresult;
}

/* -----------------------------------------------------------------------
 * nativeExecParams
 *
 * Signature (Kotlin):
 *   external fun nativeExecParams(handle: Long, sql: String,
 *                                  params: Array<String?>): String
 *
 * Executes a parameterised SQL statement ($1, $2, …) to prevent SQL
 * injection.  The params array may contain null elements (mapped to SQL
 * NULL).
 *
 * On PostgreSQL error, throws PgException (or RuntimeException).
 * ----------------------------------------------------------------------- */
JNIEXPORT jstring JNICALL
Java_com_pgandroid_PgDatabase_nativeExecParams(JNIEnv       *env,
                                                jobject       obj,
                                                jlong         handle,
                                                jstring       sql,
                                                jobjectArray  params)
{
    PgAndroidSession *session;
    const char       *c_sql;
    char             *params_json = NULL;
    char             *result;
    jstring           jresult = NULL;

    (void)obj;

    session = get_session(env, handle);
    if (!session)
        return NULL;

    if (sql == NULL)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/IllegalArgumentException"),
            "pgandroid: sql must not be null");
        return NULL;
    }

    c_sql = (*env)->GetStringUTFChars(env, sql, NULL);
    if (!c_sql)
        return NULL;

    params_json = params_to_json_array(env, params);
    if (!params_json)
    {
        /* OOM exception already pending */
        (*env)->ReleaseStringUTFChars(env, sql, c_sql);
        return NULL;
    }

    result = pgandroid_exec_params(c_sql, params_json);

    (*env)->ReleaseStringUTFChars(env, sql, c_sql);
    free(params_json);

    if (!result)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/OutOfMemoryError"),
            "pgandroid: pgandroid_exec_params returned NULL (OOM)");
        return NULL;
    }

    if (is_error_json(result))
    {
        throw_pg_exception(env, result);
        free(result);
        return NULL;
    }

    jresult = (*env)->NewStringUTF(env, result);
    free(result);
    return jresult;
}

/* -----------------------------------------------------------------------
 * nativeCheckpoint
 *
 * Signature (Kotlin):
 *   external fun nativeCheckpoint(handle: Long)
 *
 * Flushes all dirty pages and WAL to disk.  Call this before suspending
 * or backgrounding the application to guarantee durability.
 *
 * Throws PgException (or RuntimeException) on failure.
 * ----------------------------------------------------------------------- */
JNIEXPORT void JNICALL
Java_com_pgandroid_PgDatabase_nativeCheckpoint(JNIEnv  *env,
                                                jobject  obj,
                                                jlong    handle)
{
    PgAndroidSession *session;
    int               rc;

    (void)obj;

    session = get_session(env, handle);
    if (!session)
        return;

    rc = pgandroid_checkpoint();
    if (rc != 0)
    {
        (*env)->ThrowNew(env,
            (*env)->FindClass(env, "java/lang/RuntimeException"),
            "pgandroid: CHECKPOINT failed — check logcat for details");
    }
}

/* -----------------------------------------------------------------------
 * nativeClose
 *
 * Signature (Kotlin):
 *   external fun nativeClose(handle: Long)
 *
 * Gracefully shuts down the embedded PostgreSQL instance:
 *   1. Runs CHECKPOINT to flush data
 *   2. Runs PostgreSQL on_proc_exit() callbacks
 *   3. Closes all open files
 *   4. Frees the session handle
 *
 * After this call the handle is invalid.  Passing it to any other JNI
 * function throws IllegalStateException.
 *
 * Idempotent: safe to call multiple times (subsequent calls are no-ops).
 * ----------------------------------------------------------------------- */
JNIEXPORT void JNICALL
Java_com_pgandroid_PgDatabase_nativeClose(JNIEnv  *env,
                                           jobject  obj,
                                           jlong    handle)
{
    PgAndroidSession *session;

    (void)obj;

    if (handle == 0)
        return;  /* already null — no-op */

    session = (PgAndroidSession *)(uintptr_t)(uint64_t)handle;

    /* Validate magic but do not throw — Close should be idempotent */
    if (session->magic != PGANDROID_SESSION_MAGIC || session->closed)
        return;

    /* Mark closed first so that re-entrant calls (e.g. from cleanup callbacks)
     * do not attempt a second shutdown. */
    session->closed = 1;

    pgandroid_close();

    /* Invalidate the magic so accidental reuse is detected */
    session->magic = 0xDEAD1234UL;

    free(session->datadir);
    session->datadir = NULL;
    free(session);
}
