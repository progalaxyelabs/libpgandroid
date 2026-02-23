/*
 * libpgandroid.h — public C API for libpgandroid.so
 *
 * Include this header from Android/JNI code (e.g. pgandroid_jni.c) that
 * calls into the embedded PostgreSQL instance.
 *
 * All functions are safe to call from a JNI thread.  PostgreSQL runs
 * single-process/single-threaded inside the .so; the caller is responsible
 * for serialising concurrent calls (e.g. via a Kotlin Mutex / Java lock).
 *
 * Return values that are char* are malloc-allocated JSON strings.
 * The caller must free() them after copying into a Java String.
 */

#ifndef LIBPGANDROID_H
#define LIBPGANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PGANDROID_EXPORT
#  define PGANDROID_EXPORT __attribute__((visibility("default")))
#endif

/*
 * pgandroid_init — initialise PostgreSQL and run initdb if needed.
 *
 * datadir: path to the PostgreSQL data directory on Android internal storage,
 *          e.g. "/data/data/com.example.app/files/pgdata"
 *
 * Returns 0 on success, -1 on error.
 * Idempotent: safe to call multiple times; subsequent calls return 0.
 */
PGANDROID_EXPORT int pgandroid_init(const char *datadir);

/*
 * pgandroid_exec — execute a SQL statement and return a JSON result.
 *
 * sql: null-terminated UTF-8 SQL string.
 *
 * Returns a malloc-allocated JSON string:
 *   {"status":"ok","rows":[...],"fields":[...],"rowCount":N}
 *   {"status":"error","message":"...","sqlstate":"XXXXX"}
 *
 * The caller must free() the returned string after use.
 * Returns NULL only on catastrophic OOM failure.
 */
PGANDROID_EXPORT char *pgandroid_exec(const char *sql);

/*
 * pgandroid_exec_params — execute SQL with JSON-encoded parameters.
 *
 * sql:         null-terminated UTF-8 SQL string (may contain $1, $2 ... placeholders).
 * params_json: JSON array of parameter values as strings, e.g. ["42","hello"]
 *              Pass "[]" or NULL for no parameters.
 *
 * Returns malloc-allocated JSON (same format as pgandroid_exec).
 * The caller must free() the returned string after use.
 */
PGANDROID_EXPORT char *pgandroid_exec_params(const char *sql, const char *params_json);

/*
 * pgandroid_close — graceful PostgreSQL shutdown.
 *
 * Runs PostgreSQL exit hooks (checkpoint, WAL flush) then returns.
 * After this call pgandroid_init() must be called again before any exec.
 * Idempotent: safe to call when not initialised.
 */
PGANDROID_EXPORT void pgandroid_close(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBPGANDROID_H */
