# pgAndroid — Performance Benchmarks & Crash Recovery Test Plan

**Library:** libpgandroid.so (PostgreSQL 17.5, single-process ARM64)
**Test date:** 2026-02-22
**Status:** Test plan documented; awaiting physical device execution

---

## Table of Contents

1. [Target Device Specification](#1-target-device-specification)
2. [Test Environment Setup](#2-test-environment-setup)
3. [Performance Benchmark Results](#3-performance-benchmark-results)
4. [Memory Profiling Results](#4-memory-profiling-results)
5. [Crash Recovery Results](#5-crash-recovery-results)
6. [Stress Test Results](#6-stress-test-results)
7. [Storage Measurement Results](#7-storage-measurement-results)
8. [How to Run These Benchmarks](#8-how-to-run-these-benchmarks)
9. [Benchmark Code Reference](#9-benchmark-code-reference)

---

## 1. Target Device Specification

### Primary Test Device (Budget Android Phone)

| Property | Value |
|----------|-------|
| Device name | *TBD — connect via `adb devices` before running* |
| Android version | API 29+ required (Android 10+) |
| CPU class | Helio G85 / Snapdragon 665 (budget tier) |
| CPU cores | 8 cores (2× Cortex-A75 @2.0 GHz + 6× Cortex-A55 @1.8 GHz typical) |
| RAM | 3–4 GB typical |
| Storage | eMMC 5.1 (budget tier) |
| Architecture | arm64-v8a |

> **Note (2026-02-22):** No physical device was connected at test time (`adb devices` returned empty).
> No Android emulator was running. The sections below contain the test plan and performance
> targets from the HLD. Fill in the **Result** column when running on a real device.

### Reference Emulator (for CI)

| Property | Value |
|----------|-------|
| AVD names available | `RestaurantApp_Test`, `Pixel_7` |
| API level | 34 |
| Architecture | x86_64 (JIT, not ARM — results not representative of real device) |
| Host machine | Linux x86_64, 6.17.0-14-generic |

---

## 2. Test Environment Setup

### Prerequisites

```bash
# 1. Connect device via USB, enable USB debugging
adb devices    # Should show device serial

# 2. Verify architecture
adb shell getprop ro.product.cpu.abi    # Should be arm64-v8a

# 3. Install test APK
cd test-app
./gradlew assembleDebug assembleAndroidTest
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk

# 4. Run instrumented tests
adb shell am instrument -w \
    com.pgandroid.test/androidx.test.runner.AndroidJUnitRunner
```

### pgandroid Config Used for Benchmarks

```kotlin
PgConfig(
    sharedBuffers = "16MB",   // Mobile-tuned (server default: 128MB)
    workMem = "2MB",
    walBuffers = "1MB",
    maxWalSize = "64MB"
)
```

---

## 3. Performance Benchmark Results

Performance targets are from HLD §9.3. All times are wall-clock as measured by
`System.nanoTime()` in the Kotlin benchmark test.

### 3.1 Startup

| Benchmark | Target | Result | Pass/Fail | Notes |
|-----------|--------|--------|-----------|-------|
| Cold start — first `initdb` (fresh data directory) | < 5 s | *TBD* | *TBD* | Equivalent to `initdb` on a new cluster |
| Warm start — reopen existing cluster | < 1 s | *TBD* | *TBD* | Data directory already initialised |

### 3.2 Single DML Operations

| Benchmark | Target | Result | Pass/Fail | Notes |
|-----------|--------|--------|-----------|-------|
| Single `INSERT` (auto-commit) | < 2 ms | *TBD* | *TBD* | One row, fsync=on |
| Single `SELECT` by primary key | < 1 ms | *TBD* | *TBD* | Index scan, 0 rows scanned |

### 3.3 Bulk Operations

| Benchmark | Target | Result | Pass/Fail | Notes |
|-----------|--------|--------|-----------|-------|
| 1 000 `INSERT`s in a single transaction | < 1 s | *TBD* | *TBD* | One `BEGIN`/`COMMIT` wrapper |
| `SELECT` with B-tree index on 5 000 rows | < 5 ms | *TBD* | *TBD* | `WHERE id = $1` after `CREATE INDEX` |

### 3.4 PL/pgSQL

| Benchmark | Target | Result | Pass/Fail | Notes |
|-----------|--------|--------|-----------|-------|
| `CREATE OR REPLACE FUNCTION` (multi-table JOIN + aggregation body) | < 50 ms | *TBD* | *TBD* | Parse + compile only |
| Call complex PL/pgSQL function (multi-table JOIN + aggregation on 5 000 rows) | < 100 ms | *TBD* | *TBD* | Includes execution |

### 3.5 Maintenance

| Benchmark | Target | Result | Pass/Fail | Notes |
|-----------|--------|--------|-----------|-------|
| `VACUUM` on 10 MB database | < 500 ms | *TBD* | *TBD* | After bulk inserts, no autovacuum |

---

## 4. Memory Profiling Results

Measured via `android.os.Debug.MemoryInfo` / `adb shell dumpsys meminfo <package>` at each
checkpoint. RSS = Proportional Set Size reported by the OS.

| Checkpoint | RSS Reading | Notes |
|------------|-------------|-------|
| App start (before `PgDatabase.open()`) | *TBD* | Baseline |
| After `initdb` — empty cluster open | *TBD* | Shared buffers (16 MB) allocated |
| After loading 5 000 product rows | *TBD* | Table data in shared_buffers |
| During complex PL/pgSQL query | *TBD* | work_mem in use |
| After query completes (idle) | *TBD* | Does RSS return to post-init baseline? |

**Expected behaviour:** PostgreSQL allocates `shared_buffers` (16 MB) on open and keeps it
for the session lifetime. Working memory (`work_mem` 2 MB) is allocated per query and freed
after. Total expected RSS delta from baseline: ~20–25 MB.

---

## 5. Crash Recovery Results

### Test 5.1 — Clean Close / Reopen

| Step | Expected | Result | Pass/Fail |
|------|----------|--------|-----------|
| Insert row, call `checkpoint()`, call `close()` | — | *TBD* | *TBD* |
| Reopen same data directory | Data visible | *TBD* | *TBD* |
| Verify row content matches | Row = `'persistent'` | *TBD* | *TBD* |

*This test is already automated in `PgAndroidTests.testCrashRecovery()`.*

### Test 5.2 — Force-Kill Crash (Real Device Required)

```bash
# Step 1: Insert data and leave transaction OPEN (do NOT commit)
adb shell am start -n com.pgandroid.test/.BenchmarkActivity --es test open_uncommitted
# Step 2: Force-kill the app process
adb shell am force-stop com.pgandroid.test
# Step 3: Restart app — PostgreSQL WAL replay runs on open
adb shell am start -n com.pgandroid.test/.BenchmarkActivity --es test verify_rollback
```

| Scenario | Expected | Result | Pass/Fail |
|----------|----------|--------|-----------|
| Open uncommitted transaction → force-kill → reopen | **No partial data** (rollback via WAL) | *TBD* | *TBD* |
| Committed data → force-kill → reopen | **Data present** (WAL replay) | *TBD* | *TBD* |

> **Why this works:** pgAndroid has `fsync = on`, `synchronous_commit = on`, and
> `full_page_writes = on` (HLD §8.3). WAL is written to the app-private filesystem before
> each COMMIT. On next `open()`, PostgreSQL automatically replays any un-checkpointed WAL,
> recovering committed data and discarding uncommitted transactions.

---

## 6. Stress Test Results

### Scenario: Busy-Hour Simulation (50 invoices × 10 items)

This simulates a small business processing one hour of sales — roughly the expected
real-world load for `medstoreapp`.

```sql
-- Schema used for stress test
CREATE TABLE IF NOT EXISTS stress_products (
    id      SERIAL PRIMARY KEY,
    name    TEXT NOT NULL,
    price   NUMERIC(10,2) NOT NULL
);
CREATE TABLE IF NOT EXISTS stress_invoices (
    id          SERIAL PRIMARY KEY,
    created_at  TIMESTAMPTZ DEFAULT now(),
    total       NUMERIC(10,2)
);
CREATE TABLE IF NOT EXISTS stress_invoice_items (
    id          SERIAL PRIMARY KEY,
    invoice_id  INTEGER REFERENCES stress_invoices(id),
    product_id  INTEGER REFERENCES stress_products(id),
    qty         INTEGER,
    unit_price  NUMERIC(10,2)
);
```

| Metric | Target | Result | Pass/Fail |
|--------|--------|--------|-----------|
| Insert 50 products | — | *TBD* | *TBD* |
| Create 50 invoices with 10 items each (500 items total) | — | *TBD* | *TBD* |
| Total elapsed time for all 500 invoice-item INSERTs | < 5 s | *TBD* | *TBD* |
| Memory trend during stress (RSS increase %) | < 20% over baseline | *TBD* | *TBD* |
| `VACUUM` on stress database after load | < 500 ms | *TBD* | *TBD* |

---

## 7. Storage Measurement Results

```sql
-- Measure database size on device
SELECT pg_size_pretty(pg_database_size(current_database())) AS db_size;
```

| Dataset | Measured Size | HLD Estimate | Within Budget? |
|---------|--------------|--------------|----------------|
| Empty cluster (post-initdb) | *TBD* | ~8 MB | — |
| 5 000 product rows | *TBD* | ~5 MB | — |
| 500 invoice + 5 000 invoice-item rows | *TBD* | ~10 MB | — |
| 5 000 products + 500 invoices (combined) | *TBD* | ~15 MB | — |
| **2-year data estimate (extrapolated)** | *TBD* | **80–150 MB** (HLD §9.3) | *TBD* |

> **HLD reference (§9.3):** "80–150 MB for 2 years of data" is the target storage budget
> for the typical `medstoreapp` use case. PostgreSQL overhead (WAL, system catalogs, indexes)
> typically adds 2–3× the raw data size.

---

## 8. How to Run These Benchmarks

### 8.1 Automated Benchmarks via Test APK

The `BenchmarkTests` instrumented test class (to be added to `test-app`) runs all timed
scenarios and writes results to `logcat`:

```bash
# Build and install
cd test-app
./gradlew assembleDebug assembleAndroidTest

adb install -r app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk

# Run only benchmark tests
adb shell am instrument -w \
  -e class com.pgandroid.test.BenchmarkTests \
  com.pgandroid.test/androidx.test.runner.AndroidJUnitRunner \
  2>&1 | tee benchmark-run.txt
```

### 8.2 Crash Recovery (Force-Kill Test)

Force-kill requires `adb shell am force-stop` — cannot be automated via standard JUnit
instrumented tests (the test process and app process are different). Use a manual runbook:

```bash
# Terminal 1: watch logcat
adb logcat -s pgandroid:* | grep -E "(BENCH|CRASH|OPEN|CLOSE)"

# Terminal 2: manual steps
adb shell am start -n com.pgandroid.test/.BenchmarkActivity
# Follow on-screen prompts for crash recovery scenario
```

### 8.3 Recording Results

After running, fill in the *Result* and *Pass/Fail* columns in sections 3–7 above, then
commit:

```bash
git add docs/BENCHMARKS.md
git commit -m "docs: record real-device benchmark results — <device name>"
```

---

## 9. Benchmark Code Reference

The following test class should be added to
`test-app/app/src/androidTest/kotlin/com/pgandroid/test/BenchmarkTests.kt`:

```kotlin
package com.pgandroid.test

import android.os.Debug
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.pgandroid.PgDatabase
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

private const val TAG = "pgandroid-BENCH"

@RunWith(AndroidJUnit4::class)
class BenchmarkTests {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var db: PgDatabase

    @Before fun setup() {
        db = PgDatabase.open(context, "bench")
    }

    @After fun teardown() {
        try { db.close() } catch (_: Exception) {}
    }

    // -----------------------------------------------------------------------
    // B1 — Cold start (measure in BenchmarkActivity, not here)
    // -----------------------------------------------------------------------

    @Test fun benchWarmStart() {
        db.close()
        val t0 = System.nanoTime()
        db = PgDatabase.open(context, "bench")
        val ms = (System.nanoTime() - t0) / 1_000_000L
        Log.i(TAG, "BENCH warm_start_ms=$ms target=1000")
        assert(ms < 1000) { "Warm start $ms ms > 1000 ms target" }
    }

    @Test fun benchSingleInsert() {
        db.execute("DROP TABLE IF EXISTS bench_single")
        db.execute("CREATE TABLE bench_single (id SERIAL, v TEXT)")
        val t0 = System.nanoTime()
        db.execute("INSERT INTO bench_single (v) VALUES ('x')")
        val us = (System.nanoTime() - t0) / 1_000L
        Log.i(TAG, "BENCH single_insert_us=$us target=2000")
        assert(us < 2_000) { "Single INSERT $us µs > 2000 µs target" }
        db.execute("DROP TABLE bench_single")
    }

    @Test fun benchBulkInsert1000() {
        db.execute("DROP TABLE IF EXISTS bench_bulk")
        db.execute("CREATE TABLE bench_bulk (id SERIAL, v TEXT)")
        val t0 = System.nanoTime()
        db.execute("BEGIN")
        repeat(1000) { i -> db.execute("INSERT INTO bench_bulk (v) VALUES ('row$i')") }
        db.execute("COMMIT")
        val ms = (System.nanoTime() - t0) / 1_000_000L
        Log.i(TAG, "BENCH bulk_insert_1000_ms=$ms target=1000")
        assert(ms < 1000) { "1000 INSERTs $ms ms > 1000 ms target" }
        db.execute("DROP TABLE bench_bulk")
    }

    @Test fun benchSelectIndex5000() {
        db.execute("DROP TABLE IF EXISTS bench_idx")
        db.execute("CREATE TABLE bench_idx (id SERIAL PRIMARY KEY, v TEXT)")
        db.execute("BEGIN")
        repeat(5000) { i -> db.execute("INSERT INTO bench_idx (v) VALUES ('row$i')") }
        db.execute("COMMIT")
        val t0 = System.nanoTime()
        db.execute("SELECT * FROM bench_idx WHERE id = 2500")
        val us = (System.nanoTime() - t0) / 1_000L
        Log.i(TAG, "BENCH select_index_us=$us target=5000")
        assert(us < 5_000) { "Indexed SELECT $us µs > 5000 µs target" }
        db.execute("DROP TABLE bench_idx")
    }

    @Test fun benchCreateFunction() {
        val sql = """
            CREATE OR REPLACE FUNCTION bench_complex_fn(p_limit INT)
            RETURNS TABLE(product_id INT, total_qty BIGINT, revenue NUMERIC)
            LANGUAGE plpgsql AS ${"$"}${"$"}
            DECLARE
                v_cursor REFCURSOR;
            BEGIN
                RETURN QUERY
                    SELECT
                        ii.product_id,
                        SUM(ii.qty)            AS total_qty,
                        SUM(ii.qty * ii.unit_price) AS revenue
                    FROM bench_invoice_items ii
                    JOIN bench_invoices      inv ON inv.id = ii.invoice_id
                    GROUP BY ii.product_id
                    ORDER BY revenue DESC
                    LIMIT p_limit;
            END;
            ${"$"}${"$"}
        """.trimIndent()
        val t0 = System.nanoTime()
        db.execute(sql)
        val ms = (System.nanoTime() - t0) / 1_000_000L
        Log.i(TAG, "BENCH create_function_ms=$ms target=50")
        assert(ms < 50) { "CREATE FUNCTION $ms ms > 50 ms target" }
    }

    @Test fun benchVacuum() {
        db.execute("DROP TABLE IF EXISTS bench_vac")
        db.execute("CREATE TABLE bench_vac (id SERIAL, v TEXT)")
        db.execute("BEGIN")
        repeat(10000) { i -> db.execute("INSERT INTO bench_vac (v) VALUES ('${"x".repeat(100)}$i')") }
        db.execute("COMMIT")
        // Delete half to create dead tuples
        db.execute("DELETE FROM bench_vac WHERE id % 2 = 0")
        val t0 = System.nanoTime()
        db.execute("VACUUM bench_vac")
        val ms = (System.nanoTime() - t0) / 1_000_000L
        Log.i(TAG, "BENCH vacuum_ms=$ms target=500")
        assert(ms < 500) { "VACUUM $ms ms > 500 ms target" }
        db.execute("DROP TABLE bench_vac")
    }

    @Test fun benchMemoryProfile() {
        val baseline = Debug.getPss()
        Log.i(TAG, "MEM baseline_kb=$baseline")

        db.execute("DROP TABLE IF EXISTS bench_mem_products")
        db.execute("CREATE TABLE bench_mem_products (id SERIAL PRIMARY KEY, name TEXT, price NUMERIC)")
        db.execute("BEGIN")
        repeat(5000) { i -> db.execute("INSERT INTO bench_mem_products (name, price) VALUES ('Product $i', ${i * 0.99})") }
        db.execute("COMMIT")

        val afterLoad = Debug.getPss()
        Log.i(TAG, "MEM after_5000_rows_kb=$afterLoad delta_kb=${afterLoad - baseline}")

        val r = db.execute("SELECT SUM(price) FROM bench_mem_products")
        val duringQuery = Debug.getPss()
        Log.i(TAG, "MEM during_query_kb=$duringQuery")
        Log.i(TAG, "MEM result=${r.rows.first().values.first()}")

        val afterQuery = Debug.getPss()
        Log.i(TAG, "MEM after_query_kb=$afterQuery")
    }
}
```

---

## Appendix A — Performance Targets Summary

| Benchmark | Target | Source |
|-----------|--------|--------|
| Cold start (first initdb) | < 5 s | HLD §9.3 |
| Warm start (reopen existing DB) | < 1 s | HLD §9.3 |
| Single INSERT | < 2 ms | HLD §9.3 |
| 1 000 INSERTs in transaction | < 1 s | Task spec |
| SELECT with index on 5 000 rows | < 5 ms | HLD §9.3 |
| Complex PL/pgSQL function call | < 100 ms | Task spec |
| CREATE OR REPLACE FUNCTION | < 50 ms | HLD §9.3 |
| VACUUM on 10 MB database | < 500 ms | Task spec |
| 2-year data storage budget | 80–150 MB | HLD §9.3 |

---

## Appendix B — Known Constraints Affecting Benchmarks

| Constraint | Impact on Benchmarks |
|------------|---------------------|
| `fsync = on` | INSERT/COMMIT times include actual disk write latency — budget phone eMMC is ~10–40 MB/s sequential |
| `synchronous_commit = on` | Each COMMIT waits for WAL flush — bottleneck is storage, not CPU |
| Single connection, no autovacuum | VACUUM must be called manually; dead tuple accumulation affects SELECT after bulk DELETE |
| 16 MB shared_buffers | 5 000-row table (~5 MB) fits in buffer pool — repeated SELECT is cache-warm |
| No JIT in PostgreSQL (disabled for mobile) | Complex PL/pgSQL relies on interpreter speed, not JIT |

---

*Document created 2026-02-22. Run on a real device and update results above.*
