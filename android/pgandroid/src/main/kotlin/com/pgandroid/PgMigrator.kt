package com.pgandroid

import android.content.res.AssetManager

/**
 * Applies pending SQL migration files from APK assets to a [PgDatabase].
 *
 * **Asset layout** (relative to [assetPrefix]):
 * ```
 * <assetPrefix>/
 * ├── init.sql          # applied as migration version 1
 * └── migrations/
 *     ├── 002.sql       # applied as migration version 2
 *     ├── 003.sql       # applied as migration version 3
 *     └── ...
 * ```
 *
 * **Schema version tracking:** the table `pgandroid_meta` stores the current
 * schema version under the key `schema_version`. [PgMigrator] creates this
 * table if it does not exist, and advances the version after each successful
 * migration.
 *
 * **Usage:**
 * ```kotlin
 * val db = PgDatabase.open(context, "main")
 * val migrator = PgMigrator(db, context.assets, "pgandroid")
 * migrator.migrateToLatest()
 * ```
 *
 * @param db          The open [PgDatabase] to migrate.
 * @param assets      The app's [AssetManager] (from `context.assets`).
 * @param assetPrefix Path prefix inside the APK assets directory. Must not
 *                    start or end with a slash.
 */
class PgMigrator(
    private val db: PgDatabase,
    private val assets: AssetManager,
    private val assetPrefix: String
) {

    /**
     * Apply all pending migrations in version order.
     *
     * Discovers available migrations from assets and runs each one whose
     * version number is greater than the stored `schema_version`. Each
     * migration is executed inside a transaction; the schema version is
     * updated after the transaction commits. If a migration fails the
     * transaction is rolled back and a [PgException] is rethrown.
     */
    fun migrateToLatest() {
        ensureMetaTable()
        val currentVersion = currentSchemaVersion()
        val pending = pendingMigrations(currentVersion)
        for ((version, assetPath) in pending) {
            applyMigration(version, assetPath)
        }
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    /** Create pgandroid_meta if not present, and seed schema_version = 0. */
    private fun ensureMetaTable() {
        db.execute(
            """
            CREATE TABLE IF NOT EXISTS pgandroid_meta (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
            """.trimIndent()
        )
        db.execute(
            """
            INSERT INTO pgandroid_meta (key, value)
            VALUES ('schema_version', '0')
            ON CONFLICT (key) DO NOTHING
            """.trimIndent()
        )
    }

    /** Read the current schema_version from pgandroid_meta. */
    private fun currentSchemaVersion(): Int {
        val result = db.execute(
            "SELECT value FROM pgandroid_meta WHERE key = 'schema_version'"
        )
        return result.rows.firstOrNull()?.get("value")?.toString()?.toIntOrNull() ?: 0
    }

    /**
     * Return a sorted list of (version, assetPath) pairs for all migrations
     * with version > [currentVersion].
     *
     * Migration files are discovered from two locations:
     *  1. `<assetPrefix>/init.sql` — always version 1.
     *  2. `<assetPrefix>/migrations/NNN.sql` — version = NNN (numeric prefix).
     */
    private fun pendingMigrations(currentVersion: Int): List<Pair<Int, String>> {
        val migrations = mutableListOf<Pair<Int, String>>()

        // init.sql is version 1
        val initPath = "$assetPrefix/init.sql"
        if (assetExists(initPath) && currentVersion < 1) {
            migrations.add(1 to initPath)
        }

        // migrations/ directory — files named NNN.sql
        val migrationsDir = "$assetPrefix/migrations"
        val files = try {
            assets.list(migrationsDir) ?: emptyArray()
        } catch (_: Exception) {
            emptyArray()
        }

        for (file in files) {
            if (!file.endsWith(".sql")) continue
            val versionStr = file.removeSuffix(".sql")
            val version = versionStr.toIntOrNull() ?: continue
            if (version > currentVersion) {
                migrations.add(version to "$migrationsDir/$file")
            }
        }

        return migrations.sortedBy { it.first }
    }

    /** Apply a single migration inside a transaction. */
    private fun applyMigration(version: Int, assetPath: String) {
        val sql = assets.open(assetPath).bufferedReader().use { it.readText() }

        db.execute("BEGIN")
        try {
            db.execute(sql)
            db.execute(
                "UPDATE pgandroid_meta SET value = \$1 WHERE key = 'schema_version'",
                version.toString()
            )
            db.execute("COMMIT")
        } catch (e: Exception) {
            try { db.execute("ROLLBACK") } catch (_: Exception) { /* best-effort */ }
            throw e
        }
    }

    /** Return true if an asset at [path] exists. */
    private fun assetExists(path: String): Boolean = try {
        assets.open(path).use { true }
    } catch (_: Exception) {
        false
    }
}
