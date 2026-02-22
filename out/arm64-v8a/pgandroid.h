/*
 * jni/pgandroid.h — Public C API for libpgandroid.so
 *
 * This header declares the C-level interface to the embedded PostgreSQL
 * instance.  It is intended for:
 *
 *   1. The JNI bridge (pgandroid_jni.c) — Android/Kotlin callers.
 *   2. Native C/C++ host code that embeds pgandroid directly without JNI.
 *   3. Unit tests written in C.
 *
 * -----------------------------------------------------------------------
 * THREAD SAFETY
 *
 * libpgandroid.so contains a single embedded PostgreSQL backend running in
 * single-user mode.  ALL calls into this API MUST be serialised by the
 * caller.  Concurrent calls from multiple threads will corrupt PostgreSQL
 * internal state and likely crash the process.
 *
 * Recommended Kotlin-side pattern:
 *
 *   private val dbMutex = Mutex()
 *   suspend fun exec(sql: String): String = dbMutex.withLock {
 *       nativeExec(handle, sql)
 *   }
 *
 * -----------------------------------------------------------------------
 * MEMORY OWNERSHIP
 *
 * Functions that return char* return a malloc-allocated UTF-8 string.
 * The caller is responsible for calling free() on the returned pointer
 * after copying it into a Java String (or any other use).
 *
 * pgandroid_exec() and pgandroid_exec_params() return NULL only on
 * catastrophic OOM; in all other cases they return a JSON string
 * (even for SQL errors — see the "status":"error" format below).
 *
 * -----------------------------------------------------------------------
 * RESULT JSON FORMAT
 *
 * Success (SELECT):
 *   {
 *     "status":   "ok",
 *     "rows":     [{"col1": "val1", "col2": "val2"}, ...],
 *     "rowcount": 42
 *   }
 *
 * Success (INSERT / UPDATE / DELETE / DDL):
 *   {
 *     "status":   "ok",
 *     "command":  "INSERT 0 1",
 *     "rowcount": 0
 *   }
 *
 * Error:
 *   {
 *     "status":   "error",
 *     "message":  "column \"foo\" does not exist",
 *     "sqlstate": "42703"
 *   }
 *
 * -----------------------------------------------------------------------
 */

#ifndef PGANDROID_H
#define PGANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PGANDROID_EXPORT
#  define PGANDROID_EXPORT __attribute__((visibility("default")))
#endif

/* -----------------------------------------------------------------------
 * pgandroid_init — initialise PostgreSQL, running initdb if needed.
 *
 * datadir  : path to the PostgreSQL data directory on the device,
 *            e.g. "/data/data/com.example.app/files/pgdata"
 *
 * Returns 0 on success, -1 on error.
 * Idempotent: subsequent calls return 0 immediately.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT int pgandroid_init(const char *datadir);

/* -----------------------------------------------------------------------
 * pgandroid_init_with_config — initialise with optional JSON configuration.
 *
 * datadir    : same as pgandroid_init().
 * config_json: JSON object with optional GUC overrides, e.g.:
 *              {"shared_buffers":"64MB","work_mem":"2MB","fsync":"off"}
 *              May be NULL or "{}".  Unknown keys are silently ignored.
 *              Note: shared_buffers is applied at first-time init only
 *              (it cannot be changed at runtime in PostgreSQL).
 *              Runtime-settable GUCs (work_mem, temp_buffers, etc.) are
 *              applied via SET immediately after startup completes.
 *
 * Returns 0 on success, -1 on error.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT int pgandroid_init_with_config(const char *datadir,
                                                 const char *config_json);

/* -----------------------------------------------------------------------
 * pgandroid_exec — execute a SQL statement.
 *
 * sql : null-terminated UTF-8 SQL string.
 *
 * Returns a malloc-allocated JSON result string (see format above).
 * The caller must free() the returned string.
 * Returns NULL only on catastrophic OOM.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT char *pgandroid_exec(const char *sql);

/* -----------------------------------------------------------------------
 * pgandroid_exec_params — execute a parameterised SQL statement.
 *
 * sql         : SQL with $1, $2, ... placeholders.
 * params_json : JSON array of parameter values as strings,
 *               e.g. ["42", "hello", null]
 *               Pass "[]" or NULL for no parameters.
 *
 * Using parameterised queries prevents SQL injection.
 *
 * Returns a malloc-allocated JSON result string.
 * The caller must free() the returned string.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT char *pgandroid_exec_params(const char *sql,
                                              const char *params_json);

/* -----------------------------------------------------------------------
 * pgandroid_checkpoint — flush all dirty data pages and WAL to disk.
 *
 * Equivalent to executing CHECKPOINT via pgandroid_exec().
 * Call this before pausing the application to ensure data durability.
 *
 * Returns 0 on success, -1 on error.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT int pgandroid_checkpoint(void);

/* -----------------------------------------------------------------------
 * pgandroid_close — graceful PostgreSQL shutdown.
 *
 * Runs PostgreSQL exit hooks (checkpoint, WAL flush, buffer manager
 * teardown) and then returns to the caller.  After this call,
 * pgandroid_init() must be invoked again before any exec.
 *
 * Idempotent: safe to call when already closed or never opened.
 * ----------------------------------------------------------------------- */
PGANDROID_EXPORT void pgandroid_close(void);

#ifdef __cplusplus
}
#endif

#endif /* PGANDROID_H */
