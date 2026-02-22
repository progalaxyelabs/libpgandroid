package com.pgandroid.test

import android.content.Context
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.pgandroid.PgDatabase
import com.pgandroid.PgMigrator
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Instrumented smoke tests for pgandroid (embedded PostgreSQL on Android).
 *
 * Run with:
 *   ./gradlew connectedAndroidTest
 *
 * Requires an Android emulator or device (API 29+, arm64-v8a or x86_64).
 *
 * Each test opens (or reuses) the "smoke_tests" database in the app's
 * private storage.  The data directory persists across tests within a single
 * test run, so `initdb` runs only once (on the first @Before call).
 * Subsequent opens merely restart the already-initialised PostgreSQL cluster.
 *
 * Tests that require a close/reopen cycle (testOpenClose, testCrashRecovery)
 * close `db` inside the test body and re-assign it; the @After teardown
 * gracefully handles the already-closed instance.
 */
@RunWith(AndroidJUnit4::class)
class PgAndroidTests {

    // Use targetContext so that assets bundled in the APP apk are accessible.
    private lateinit var context: Context
    private lateinit var db: PgDatabase

    @Before
    fun setup() {
        context = InstrumentationRegistry.getInstrumentation().targetContext
        db = PgDatabase.open(context, "smoke_tests")
    }

    @After
    fun teardown() {
        // close() is idempotent; safe if a test already closed db.
        try { db.close() } catch (_: Exception) { }
    }

    // -----------------------------------------------------------------------
    // testOpenClose — open DB, close, reopen; no crash
    // -----------------------------------------------------------------------

    @Test
    fun testOpenClose() {
        val r1 = db.execute("SELECT 1 AS val")
        assertEquals("1", r1.rows.first()["val"].toString())

        db.close()

        // Reopen the same data directory — pgandroid_init is re-entrant after close
        db = PgDatabase.open(context, "smoke_tests")

        val r2 = db.execute("SELECT 42 AS val")
        assertEquals("42", r2.rows.first()["val"].toString())
    }

    // -----------------------------------------------------------------------
    // testCreateTable — CREATE TABLE + INSERT + SELECT
    // -----------------------------------------------------------------------

    @Test
    fun testCreateTable() {
        db.execute("DROP TABLE IF EXISTS test_ct")
        db.execute(
            """
            CREATE TABLE test_ct (
                id    SERIAL          PRIMARY KEY,
                name  TEXT            NOT NULL,
                price NUMERIC(10, 2)
            )
            """.trimIndent()
        )
        db.execute("INSERT INTO test_ct (name, price) VALUES ('Apple', 1.99)")
        db.execute("INSERT INTO test_ct (name, price) VALUES ('Banana', 0.99)")

        val result = db.execute("SELECT name, price FROM test_ct ORDER BY name")

        assertEquals("Expected 2 rows", 2, result.rows.size)
        assertEquals("Apple",  result.rows[0]["name"])
        assertEquals("Banana", result.rows[1]["name"])

        db.execute("DROP TABLE test_ct")
    }

    // -----------------------------------------------------------------------
    // testPlpgsqlFunction — CREATE FUNCTION + SELECT func()
    // -----------------------------------------------------------------------

    @Test
    fun testPlpgsqlFunction() {
        db.execute("DROP TABLE IF EXISTS test_plpg")
        db.execute("CREATE TABLE test_plpg (id SERIAL PRIMARY KEY, label TEXT)")
        db.execute("INSERT INTO test_plpg (label) VALUES ('Cherry')")

        db.execute(
            """
            CREATE OR REPLACE FUNCTION get_test_plpg_labels()
            RETURNS SETOF TEXT
            LANGUAGE plpgsql AS ${"$"}${"$"}
            BEGIN
                RETURN QUERY SELECT label FROM test_plpg ORDER BY label;
            END;
            ${"$"}${"$"}
            """.trimIndent()
        )

        val result = db.execute("SELECT * FROM get_test_plpg_labels()")
        assertEquals("Expected 1 row from PL/pgSQL function", 1, result.rows.size)
        val row = result.rows.first()
        assertEquals("Cherry", row.values.first().toString())

        db.execute("DROP FUNCTION IF EXISTS get_test_plpg_labels()")
        db.execute("DROP TABLE test_plpg")
    }

    // -----------------------------------------------------------------------
    // testPgcrypto — CREATE EXTENSION pgcrypto + gen_random_uuid()
    // -----------------------------------------------------------------------

    @Test
    fun testPgcrypto() {
        db.execute("CREATE EXTENSION IF NOT EXISTS pgcrypto")

        val result = db.execute("SELECT gen_random_uuid()::TEXT AS uuid")
        val uuid = result.rows.first()["uuid"]

        assertNotNull("gen_random_uuid() should return a value", uuid)
        assertTrue(
            "gen_random_uuid() should match UUID format",
            uuid.toString().matches(UUID_REGEX)
        )
    }

    // -----------------------------------------------------------------------
    // testUuidOssp — CREATE EXTENSION uuid-ossp + uuid_generate_v4()
    // -----------------------------------------------------------------------

    @Test
    fun testUuidOssp() {
        db.execute("""CREATE EXTENSION IF NOT EXISTS "uuid-ossp"""")

        val result = db.execute("SELECT uuid_generate_v4()::TEXT AS uuid")
        val uuid = result.rows.first()["uuid"]

        assertNotNull("uuid_generate_v4() should return a value", uuid)
        assertTrue(
            "uuid_generate_v4() should match UUID v4 format",
            uuid.toString().matches(UUID_V4_REGEX)
        )
    }

    // -----------------------------------------------------------------------
    // testParameterizedQuery — SELECT * FROM t WHERE id = $1
    // -----------------------------------------------------------------------

    @Test
    fun testParameterizedQuery() {
        db.execute("DROP TABLE IF EXISTS test_params")
        db.execute("CREATE TABLE test_params (id SERIAL PRIMARY KEY, value TEXT)")
        db.execute("INSERT INTO test_params (value) VALUES (\$1)", "hello")
        db.execute("INSERT INTO test_params (value) VALUES (\$1)", "world")

        val result = db.execute(
            "SELECT value FROM test_params WHERE value = \$1",
            "hello"
        )

        assertEquals("Parameterised query should return exactly 1 row", 1, result.rows.size)
        assertEquals("hello", result.rows[0]["value"])

        db.execute("DROP TABLE test_params")
    }

    // -----------------------------------------------------------------------
    // testTransaction — BEGIN + multiple INSERTs + COMMIT + verify
    // -----------------------------------------------------------------------

    @Test
    fun testTransaction() {
        db.execute("DROP TABLE IF EXISTS test_txn")
        db.execute("CREATE TABLE test_txn (id SERIAL PRIMARY KEY, label TEXT)")

        db.execute("BEGIN")
        db.execute("INSERT INTO test_txn (label) VALUES ('one')")
        db.execute("INSERT INTO test_txn (label) VALUES ('two')")
        db.execute("INSERT INTO test_txn (label) VALUES ('three')")
        db.execute("COMMIT")

        val result = db.execute("SELECT COUNT(*) AS cnt FROM test_txn")
        assertEquals("All 3 rows should be visible after COMMIT",
            "3", result.rows.first()["cnt"].toString())

        db.execute("DROP TABLE test_txn")
    }

    // -----------------------------------------------------------------------
    // testMigration — PgMigrator runs init.sql + migrations
    // -----------------------------------------------------------------------

    @Test
    fun testMigration() {
        // init.sql is idempotent (IF NOT EXISTS / OR REPLACE), so safe to run
        // even if the products table was created by a previous test run.
        val migrator = PgMigrator(db, context.assets, "pgandroid")
        migrator.migrateToLatest()

        // pgandroid_meta must exist and hold the correct schema_version
        val meta = db.execute(
            "SELECT value FROM pgandroid_meta WHERE key = 'schema_version'"
        )
        assertTrue("pgandroid_meta must contain schema_version", meta.rows.isNotEmpty())

        val version = meta.rows.first()["value"].toString().toInt()
        assertTrue("schema_version should be >= 2 (init.sql + 002.sql)", version >= 2)

        // products table must exist (created by init.sql)
        val products = db.execute("SELECT COUNT(*) AS cnt FROM products")
        assertNotNull("products table must exist after migration", products)
    }

    // -----------------------------------------------------------------------
    // testCrashRecovery — insert, close, reopen, verify data persists
    // -----------------------------------------------------------------------

    @Test
    fun testCrashRecovery() {
        db.execute("DROP TABLE IF EXISTS test_recovery")
        db.execute("CREATE TABLE test_recovery (id SERIAL PRIMARY KEY, payload TEXT)")
        db.execute("INSERT INTO test_recovery (payload) VALUES ('persistent')")

        // Flush to disk before simulating a "crash" (graceful close)
        db.checkpoint()
        db.close()

        // Reopen the same cluster — data directory is unchanged
        db = PgDatabase.open(context, "smoke_tests")

        val result = db.execute("SELECT payload FROM test_recovery ORDER BY id")

        assertEquals("Row count after close/reopen", 1, result.rows.size)
        assertEquals("Data should survive close/reopen", "persistent", result.rows[0]["payload"])

        db.execute("DROP TABLE test_recovery")
    }

    // -----------------------------------------------------------------------
    // testJsonAgg — SELECT json_agg(row_to_json(t)) — PG-specific aggregate
    // -----------------------------------------------------------------------

    @Test
    fun testJsonAgg() {
        db.execute("DROP TABLE IF EXISTS test_jagg")
        db.execute("CREATE TABLE test_jagg (id SERIAL PRIMARY KEY, name TEXT)")
        db.execute("INSERT INTO test_jagg (name) VALUES ('a'), ('b'), ('c')")

        val result = db.execute(
            "SELECT json_agg(row_to_json(t)) AS data FROM test_jagg t"
        )

        val data = result.rows.first()["data"].toString()
        assertTrue("json_agg should return a JSON array starting with '['", data.startsWith("["))

        db.execute("DROP TABLE test_jagg")
    }

    // -----------------------------------------------------------------------
    // testCTE — WITH ... SELECT — common table expression
    // -----------------------------------------------------------------------

    @Test
    fun testCTE() {
        db.execute("DROP TABLE IF EXISTS test_cte")
        db.execute("CREATE TABLE test_cte (n INTEGER)")
        db.execute("INSERT INTO test_cte VALUES (1), (2), (3), (4), (5)")

        val result = db.execute(
            """
            WITH numbered AS (
                SELECT n, ROW_NUMBER() OVER (ORDER BY n) AS rn
                FROM test_cte
            )
            SELECT SUM(n) AS total FROM numbered
            """.trimIndent()
        )

        assertEquals("CTE SUM(1..5) should be 15",
            "15", result.rows.first()["total"].toString())

        db.execute("DROP TABLE test_cte")
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    companion object {
        /** Matches any standard UUID string (8-4-4-4-12 hex groups). */
        private val UUID_REGEX = Regex(
            "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
        )

        /** Matches a UUID v4 specifically (version nibble = 4, variant bits = 8/9/a/b). */
        private val UUID_V4_REGEX = Regex(
            "[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"
        )
    }
}
