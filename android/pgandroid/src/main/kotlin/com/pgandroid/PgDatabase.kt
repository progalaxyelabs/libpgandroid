package com.pgandroid

import android.content.Context
import java.io.Closeable
import java.io.File

/**
 * Main entry point for interacting with an embedded PostgreSQL database.
 *
 * **Thread safety:** libpgandroid.so runs a single-user PostgreSQL backend.
 * All calls MUST be serialised by the caller. Use a Kotlin `Mutex` or a
 * single-threaded coroutine dispatcher when sharing a [PgDatabase] across
 * coroutines or threads.
 *
 * **Lifecycle:**
 * ```kotlin
 * val db = PgDatabase.open(context, "main")
 * try {
 *     val result = db.execute("SELECT 1")
 * } finally {
 *     db.close()
 * }
 * ```
 * Or with `use`:
 * ```kotlin
 * PgDatabase.open(context, "main").use { db ->
 *     db.execute("CREATE TABLE ...")
 * }
 * ```
 */
class PgDatabase private constructor(private var handle: Long) : Closeable {

    companion object {
        init {
            System.loadLibrary("pgandroid")
        }

        /**
         * Open (or create) a named database in the app's private storage.
         *
         * The data directory is:
         *   `${context.filesDir}/pgandroid/<dbName>/`
         *
         * On first call PostgreSQL's `initdb` is run (takes 2–5 seconds on
         * budget hardware). Subsequent opens reuse the existing data directory.
         *
         * Extensions listed in [config] are loaded via
         * `CREATE EXTENSION IF NOT EXISTS` immediately after open.
         *
         * @param context  Android context used to resolve [Context.getFilesDir].
         * @param dbName   Logical database name; used as the directory name.
         *                 Must be a valid filesystem component (no slashes).
         * @param config   Runtime configuration. Defaults to mobile-tuned values.
         * @return         A ready-to-use [PgDatabase] instance.
         * @throws RuntimeException if PostgreSQL initialization fails.
         */
        fun open(
            context: Context,
            dbName: String,
            config: PgConfig = PgConfig.default()
        ): PgDatabase {
            val dataDir = File(context.filesDir, "pgandroid/$dbName")
            dataDir.mkdirs()

            val handle = nativeOpen(dataDir.absolutePath, config.toJson())

            val db = PgDatabase(handle)

            // Load any additional extensions beyond pgcrypto (which nativeOpen
            // already installs).  pgcrypto is filtered out to avoid a redundant
            // CREATE EXTENSION call.
            for (ext in config.extensions) {
                if (ext != "pgcrypto") {
                    try {
                        db.nativeExec(handle, "CREATE EXTENSION IF NOT EXISTS \"$ext\"")
                    } catch (_: Exception) {
                        // Extension may not be compiled in — non-fatal
                    }
                }
            }

            return db
        }

        // -----------------------------------------------------------------------
        // JNI declarations
        // -----------------------------------------------------------------------

        @JvmStatic private external fun nativeOpen(dataDir: String, configJson: String?): Long
    }

    // -----------------------------------------------------------------------
    // JNI declarations (instance-level to match JNI method signatures)
    // -----------------------------------------------------------------------

    private external fun nativeExec(handle: Long, sql: String): String
    private external fun nativeExecParams(handle: Long, sql: String, params: Array<String?>): String
    private external fun nativeCheckpoint(handle: Long)
    private external fun nativeClose(handle: Long)

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /**
     * Execute a SQL statement and return the result.
     *
     * Suitable for DDL, DML without user-supplied values, or when the caller
     * has already sanitised input. For user-supplied values prefer the
     * parameterised overload to prevent SQL injection.
     *
     * @throws PgException          if PostgreSQL returns an error.
     * @throws IllegalStateException if the database is already closed.
     */
    fun execute(sql: String): PgResult {
        checkOpen()
        val json = nativeExec(handle, sql)
        return PgResult.fromJson(json)
    }

    /**
     * Execute a parameterised SQL statement.
     *
     * Placeholders in [sql] are `$1`, `$2`, ... corresponding to [params] in
     * order. Null elements in [params] map to SQL `NULL`.
     *
     * Example:
     * ```kotlin
     * db.execute("INSERT INTO users (name, age) VALUES (\$1, \$2)", "Alice", 30)
     * ```
     *
     * @throws PgException          if PostgreSQL returns an error.
     * @throws IllegalStateException if the database is already closed.
     */
    fun execute(sql: String, vararg params: Any?): PgResult {
        checkOpen()
        val stringParams: Array<String?> = Array(params.size) { i ->
            params[i]?.toString()
        }
        val json = nativeExecParams(handle, sql, stringParams)
        return PgResult.fromJson(json)
    }

    /**
     * Call a PostgreSQL function by name and return the result as a JSON string.
     *
     * Builds and executes `SELECT <name>($1, $2, ...) AS result` with the
     * supplied parameters and returns the value of the first column of the
     * first row as a string.
     *
     * ```kotlin
     * val uuid = db.callFunction("gen_random_uuid")
     * val hash = db.callFunction("crypt", rawPassword, db.callFunction("gen_salt", "bf"))
     * ```
     *
     * @return The string representation of the function's return value.
     * @throws PgException          if PostgreSQL returns an error.
     * @throws IllegalStateException if the database is already closed or the
     *                               function returned no rows.
     */
    fun callFunction(name: String, vararg params: Any?): String {
        val placeholders = if (params.isEmpty()) "" else
            (1..params.size).joinToString(", ") { "\$$it" }
        val sql = "SELECT \"$name\"($placeholders) AS result"
        val result = execute(sql, *params)
        return result.rows.firstOrNull()?.get("result")?.toString()
            ?: throw IllegalStateException("pgandroid: function '$name' returned no rows")
    }

    /**
     * Flush all dirty data pages and WAL segments to disk.
     *
     * Call this before backgrounding or suspending the application to ensure
     * crash durability without relying on the next WAL checkpoint cycle.
     *
     * @throws RuntimeException      on CHECKPOINT failure.
     * @throws IllegalStateException if the database is already closed.
     */
    fun checkpoint() {
        checkOpen()
        nativeCheckpoint(handle)
    }

    /**
     * Gracefully shut down the embedded PostgreSQL instance.
     *
     * Runs exit hooks (checkpoint + WAL flush + buffer teardown) before
     * returning. Subsequent calls to any method on this object throw
     * [IllegalStateException].
     *
     * Idempotent: safe to call multiple times.
     */
    override fun close() {
        if (handle != 0L) {
            nativeClose(handle)
            handle = 0L
        }
    }

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    private fun checkOpen() {
        check(handle != 0L) { "pgandroid: database is already closed" }
    }
}
