# pgAndroid — High Level Design

> Native PostgreSQL embedded library for Android

## 1. Vision

Run unmodified PL/pgSQL functions on Android phones. No server, no rewrite, no SQLite translation layer. The same SQL that runs on your PostgreSQL server runs on the phone.

## 2. Problem Statement

Apps that use PostgreSQL on the server face a painful choice for offline/mobile:
- **Rewrite everything** in SQLite/Room (different SQL dialect, no PL/pgSQL, no stored functions)
- **Always require network** (defeats offline-first)
- **Use PGlite WASM** (works but: slower than native, weak crash durability, WebView dependency)

pgAndroid eliminates this choice by embedding PostgreSQL as a native ARM64 shared library.

## 3. Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│                   Android App                        │
│                                                      │
│  ┌──────────────┐     ┌───────────────────────────┐ │
│  │  App Code     │     │  WebView (Angular/React)  │ │
│  │  (Kotlin)     │     │  or native UI             │ │
│  └──────┬───────┘     └──────────┬────────────────┘ │
│         │                        │                   │
│         │    ┌───────────────────┘                   │
│         │    │                                       │
│  ┌──────▼────▼──────────────────────────────────┐   │
│  │         pgAndroid JNI Bridge (Kotlin)          │   │
│  │                                                │   │
│  │  PgDatabase.open(path)                         │   │
│  │  PgDatabase.execute(sql) -> ResultSet          │   │
│  │  PgDatabase.callFunction(name, params) -> JSON │   │
│  │  PgDatabase.close()                            │   │
│  └──────────────────┬───────────────────────────┘   │
│                     │ JNI                            │
│  ┌──────────────────▼───────────────────────────┐   │
│  │           libpgandroid.so (native C)           │   │
│  │                                                │   │
│  │  PostgreSQL 17.5 (single-process patches)      │   │
│  │  ├── Query executor                            │   │
│  │  ├── PL/pgSQL interpreter                      │   │
│  │  ├── WAL (write-ahead log)                     │   │
│  │  ├── Buffer manager (shared_buffers via malloc) │   │
│  │  ├── pgcrypto extension                        │   │
│  │  └── uuid-ossp extension                       │   │
│  └──────────────────┬───────────────────────────┘   │
│                     │ read/write                     │
│  ┌──────────────────▼───────────────────────────┐   │
│  │     App-Private Filesystem                     │   │
│  │     /data/data/com.app/files/pgdata/           │   │
│  │     ├── base/          (table data)            │   │
│  │     ├── pg_wal/        (WAL files)             │   │
│  │     └── pg_stat_tmp/   (stats)                 │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## 4. Components

### 4.1 libpgandroid.so — Core Native Library

**Source**: PostgreSQL 17.5 from `electric-sql/postgres-pglite` fork, branch `REL_17_5_WASM`.

**Key modifications over upstream PostgreSQL**:

| Area | Upstream Behavior | pgAndroid Behavior |
|------|-------------------|-------------------|
| Process model | Multi-process (postmaster forks workers) | Single-process, single-connection |
| Shared memory | System V shmget() | malloc() — one process, nothing to share |
| IPC | Unix domain sockets | Direct function calls via JNI |
| Semaphores | POSIX sem_wait loops | Single sem_trywait, no contention |
| proc_exit() | Calls exit() | Returns control to host (Kotlin) |
| Communication | Socket-based wire protocol | In-memory buffer exchange |

**Key modifications over PGlite's WASM patches**:

| Area | PGlite WASM | pgAndroid |
|------|-------------|-----------|
| Compile target | Emscripten → WASM | Android NDK clang → ARM64 ELF .so |
| Platform guard | `__EMSCRIPTEN__ \|\| __wasi__` | `__ANDROID_PGANDROID__` |
| I/O interface | JavaScript interop | JNI (Java Native Interface) |
| Storage | IndexedDB / OPFS | Direct filesystem (app-private dir) |
| WAL | In WASM memory (weak durability) | On-disk (full crash recovery) |
| Entry point | pg_main.c → JS callbacks | pg_main.c → JNI exports |

**Preprocessor strategy**: Add `__ANDROID_PGANDROID__` alongside existing `__EMSCRIPTEN__` / `__wasi__` guards. Reuse the same single-process logic, only change the I/O and entry point layer.

### 4.2 JNI Bridge (C side)

Thin C layer that exposes PostgreSQL to Java/Kotlin via JNI. Located in `jni/pgandroid_jni.c`.

**Exported JNI functions**:

```c
// Initialize PostgreSQL (create data directory, run initdb if first run)
JNIEXPORT jlong JNICALL Java_com_pgandroid_PgDatabase_nativeOpen(
    JNIEnv *env, jobject obj, jstring dataDir, jstring configJson);

// Execute SQL, return result as JSON string
JNIEXPORT jstring JNICALL Java_com_pgandroid_PgDatabase_nativeExec(
    JNIEnv *env, jobject obj, jlong handle, jstring sql);

// Execute SQL with parameters (prevents SQL injection)
JNIEXPORT jstring JNICALL Java_com_pgandroid_PgDatabase_nativeExecParams(
    JNIEnv *env, jobject obj, jlong handle, jstring sql, jobjectArray params);

// Run CHECKPOINT
JNIEXPORT void JNICALL Java_com_pgandroid_PgDatabase_nativeCheckpoint(
    JNIEnv *env, jobject obj, jlong handle);

// Graceful shutdown
JNIEXPORT void JNICALL Java_com_pgandroid_PgDatabase_nativeClose(
    JNIEnv *env, jobject obj, jlong handle);
```

### 4.3 Kotlin API (Android side)

High-level Kotlin wrapper distributed as an `.aar`. Located in `android/pgandroid/`.

```kotlin
class PgDatabase private constructor(private val handle: Long) {

    companion object {
        init { System.loadLibrary("pgandroid") }

        fun open(context: Context, dbName: String = "main",
                 config: PgConfig = PgConfig.default()): PgDatabase
    }

    // Execute raw SQL
    fun execute(sql: String): PgResult

    // Execute with parameters (safe from injection)
    fun execute(sql: String, vararg params: Any?): PgResult

    // Call a PostgreSQL function by name, return JSON
    fun callFunction(name: String, vararg params: Any?): String

    // Run a migration SQL file
    fun migrate(sql: String)

    // Checkpoint WAL to disk
    fun checkpoint()

    // Graceful shutdown
    fun close()
}

data class PgConfig(
    val sharedBuffers: String = "16MB",
    val workMem: String = "2MB",
    val walBuffers: String = "1MB",
    val maxWalSize: String = "64MB",
    val extensions: List<String> = listOf("pgcrypto", "uuid-ossp")
)

data class PgResult(
    val columns: List<String>,
    val rows: List<Map<String, Any?>>,
    val rowsAffected: Int
)
```

### 4.4 Extensions

Statically compiled into libpgandroid.so. No dynamic loading.

| Extension | Purpose | Size Impact |
|-----------|---------|-------------|
| pgcrypto | gen_random_uuid(), password hashing | +1 MB (needs OpenSSL) |
| uuid-ossp | uuid_generate_v4() | +20 KB |
| pg_trgm | Fuzzy text search (future) | +100 KB |

Extensions are activated per-database via `CREATE EXTENSION pgcrypto;` — same as server PostgreSQL.

## 5. Build System

### 5.1 Build Pipeline

```
Step 1: Cross-compile OpenSSL for aarch64-linux-android
        ↓
Step 2: Apply pgAndroid patches to postgres-pglite source
        ↓
Step 3: Configure PostgreSQL with NDK toolchain
        ./configure \
          --host=aarch64-linux-android34 \
          --prefix=/output \
          CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang \
          --without-readline \
          --without-zlib \
          --disable-nls \
          --with-openssl=/path/to/cross-compiled-openssl \
          CFLAGS="-D__ANDROID_PGANDROID__ -Os -fPIC"
        ↓
Step 4: make -j$(nproc) (cross-compile)
        ↓
Step 5: Extract libpgandroid.so from build artifacts
        ↓
Step 6: Compile JNI bridge against libpgandroid
        ↓
Step 7: Package as .aar (optional, for distribution)
```

### 5.2 Build Requirements

| Tool | Version | Location |
|------|---------|----------|
| Android NDK | r27+ | /hdd1/android-dev/sdk/ndk/27.x/ |
| GNU Make | 4.0+ | System |
| Autoconf | 2.69+ | System |
| Git | 2.0+ | System |
| GCC/Clang (host) | Any recent | System (for build-time tools only) |

**No Android Studio, no Gradle, no Java needed for the .so build.**

Gradle is only needed if packaging the Kotlin wrapper as an .aar or building the test APK.

### 5.3 Build Script

A single `build.sh` at project root orchestrates the entire pipeline:

```
./build.sh                    # Build for arm64-v8a (default)
./build.sh --arch x86_64      # Build for emulator
./build.sh --clean            # Clean and rebuild
./build.sh --test             # Build + run tests on connected device/emulator
```

### 5.4 Output Artifacts

```
out/
├── arm64-v8a/
│   ├── libpgandroid.so       (~15-20 MB, stripped)
│   └── libpgandroid.h        (C API header)
├── x86_64/
│   └── libpgandroid.so       (emulator build)
└── pgandroid.aar             (Kotlin wrapper + .so, for Maven/Gradle)
```

## 6. Data Directory Layout

On first `PgDatabase.open()`, pgAndroid initializes a PostgreSQL data directory in app-private storage:

```
/data/data/com.example.app/files/pgandroid/main/
├── PG_VERSION                  # "17"
├── postgresql.conf             # Mobile-tuned settings
├── base/
│   └── 1/                      # Database OID
│       ├── ... (table files)
│       └── ... (index files)
├── global/
│   └── ... (system catalogs)
├── pg_wal/
│   └── 000000010000000000000001  # WAL segment (16 MB max)
└── pg_stat_tmp/
    └── ... (stats collector temp)
```

This initialization is equivalent to `initdb` — done once, takes ~2-3 seconds on a budget phone.

## 7. Migration System

### 7.1 Schema Version Tracking

pgAndroid creates a metadata table on first init:

```sql
CREATE TABLE IF NOT EXISTS pgandroid_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
INSERT INTO pgandroid_meta (key, value)
VALUES ('schema_version', '0')
ON CONFLICT DO NOTHING;
```

### 7.2 Migration Flow

App developers bundle SQL migration files in APK assets:

```
assets/pgandroid/
├── init.sql                    # Initial schema + functions (version 1)
└── migrations/
    ├── 002.sql                 # Schema changes for version 2
    ├── 003.sql                 # Schema changes for version 3
    └── ...
```

On app startup:

```kotlin
val db = PgDatabase.open(context, "main")
val migrator = PgMigrator(db, context.assets, "pgandroid")
migrator.migrateToLatest()  // Runs pending migrations in order
```

### 7.3 Function Updates

PL/pgSQL functions use `CREATE OR REPLACE FUNCTION` — idempotent by design. The recommended pattern is to reload all functions after migrations:

```kotlin
// migrations/003.sql
-- Add new column
ALTER TABLE products ADD COLUMN IF NOT EXISTS barcode TEXT;

-- Reload affected function (CREATE OR REPLACE is safe)
CREATE OR REPLACE FUNCTION search_products(p_query TEXT)
RETURNS TABLE(...) AS $$ ... $$ LANGUAGE plpgsql;
```

## 8. Platform Constraints

### 8.1 Android Version

**Minimum: API 29 (Android 10)**. Required for `shm_open()` in bionic libc. Covers 90%+ of active Android devices as of 2026.

### 8.2 Permissions

**None required.** pgAndroid uses only app-private storage (`context.filesDir`). No STORAGE, INTERNET, or any other permission needed.

### 8.3 Resource Defaults (Mobile-Tuned)

```
shared_buffers = 16MB        # Server default: 128MB
work_mem = 2MB               # Server default: 4MB
wal_buffers = 1MB            # Server default: -1 (auto)
max_wal_size = 64MB          # Server default: 1GB
min_wal_size = 16MB          # Server default: 80MB
maintenance_work_mem = 8MB   # Server default: 64MB
effective_cache_size = 32MB  # Server default: 4GB
fsync = on                   # Crash safety (non-negotiable)
synchronous_commit = on      # Durability guarantee
full_page_writes = on        # Crash safety
```

### 8.4 Limitations

| Limitation | Reason | Impact |
|------------|--------|--------|
| Single connection only | No postmaster, no fork() | Fine for single-user mobile apps |
| No LISTEN/NOTIFY | Requires background workers | Use Android LiveData/Flow instead |
| No autovacuum | Requires background launcher | Call VACUUM manually or on app start |
| No streaming replication | Single-process | Not relevant for mobile |
| No dynamic extension loading | Statically compiled | Must decide extensions at build time |
| No locale support (C/POSIX only) | bionic libc limitation | Use application-level collation if needed |

## 9. Testing Strategy

### 9.1 Unit Tests (Host Machine)

Cross-compile for x86_64 Linux (not Android), run PostgreSQL regression tests:

```bash
./build.sh --target linux-x86_64 --test
```

This validates that the single-process patches work correctly without needing an Android device.

### 9.2 Integration Tests (Android Emulator)

Minimal test APK that exercises the JNI bridge:

```kotlin
@Test fun testCreateTable() {
    val db = PgDatabase.open(context, "test")
    db.execute("CREATE TABLE t (id SERIAL PRIMARY KEY, name TEXT)")
    db.execute("INSERT INTO t (name) VALUES ('hello')")
    val result = db.execute("SELECT * FROM t")
    assertEquals(1, result.rows.size)
    assertEquals("hello", result.rows[0]["name"])
    db.close()
}

@Test fun testPlpgsqlFunction() {
    val db = PgDatabase.open(context, "test")
    db.execute("""
        CREATE OR REPLACE FUNCTION add_numbers(a INT, b INT)
        RETURNS INT AS $$ BEGIN RETURN a + b; END; $$ LANGUAGE plpgsql;
    """)
    val result = db.execute("SELECT add_numbers(3, 4) AS sum")
    assertEquals(7, result.rows[0]["sum"])
    db.close()
}

@Test fun testCrashRecovery() {
    val db = PgDatabase.open(context, "test")
    db.execute("CREATE TABLE t (id SERIAL, val TEXT)")
    db.execute("INSERT INTO t (val) VALUES ('before_crash')")
    db.close()  // Simulate clean shutdown

    // Reopen — WAL replay should recover data
    val db2 = PgDatabase.open(context, "test")
    val result = db2.execute("SELECT val FROM t")
    assertEquals("before_crash", result.rows[0]["val"])
    db2.close()
}

@Test fun testPgcrypto() {
    val db = PgDatabase.open(context, "test")
    db.execute("CREATE EXTENSION IF NOT EXISTS pgcrypto")
    val result = db.execute("SELECT gen_random_uuid()::text AS uuid")
    assertTrue(result.rows[0]["uuid"].toString().matches(
        Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    ))
    db.close()
}
```

### 9.3 Performance Tests (Real Device)

Run on a budget Android phone (Helio G85 class):

- INSERT 1000 rows in a transaction — target: < 500ms
- SELECT with index on 5000 rows — target: < 5ms
- CREATE OR REPLACE FUNCTION (complex PL/pgSQL) — target: < 50ms
- Cold start (initdb) — target: < 5 seconds
- Warm start (reopen existing DB) — target: < 1 second

## 10. Distribution

### 10.1 For App Developers (Future)

```gradle
// build.gradle.kts
dependencies {
    implementation("com.pgandroid:pgandroid:1.0.0")
}
```

Published to Maven Central as an `.aar` containing:
- `libpgandroid.so` for arm64-v8a and x86_64
- Kotlin API classes
- ProGuard rules

### 10.2 For medstoreapp (Immediate Use)

Direct `.so` inclusion in the Android project:

```
app/src/main/jniLibs/arm64-v8a/libpgandroid.so
```

Plus Kotlin source files copied into the project.

## 11. Security Considerations

| Concern | Mitigation |
|---------|-----------|
| SQL injection | Parameterized queries via `nativeExecParams()` |
| Data at rest | App-private directory (Android sandbox). Optional: SQLCipher-style encryption (future) |
| Superuser access | Single-user mode runs as superuser internally, but this is contained within the app sandbox |
| Extension safety | Only vetted extensions compiled in. No dynamic loading. |
| Memory safety | PostgreSQL's battle-tested C code. No custom memory management. |

## 12. Roadmap

### Phase 1 — Core Library (Current)
- [ ] Apply Android NDK patches to postgres-pglite fork
- [ ] Cross-compile PostgreSQL for aarch64
- [ ] Cross-compile OpenSSL for aarch64
- [ ] JNI bridge (open, exec, close)
- [ ] Basic test APK on emulator
- [ ] Test on real budget Android phone

### Phase 2 — Production Hardening
- [ ] Parameterized queries
- [ ] Migration system
- [ ] VACUUM scheduling
- [ ] Memory limit configuration
- [ ] Crash recovery testing
- [ ] Performance benchmarks on budget phones

### Phase 3 — Distribution
- [ ] Package as .aar
- [ ] Publish to Maven Central
- [ ] Documentation site
- [ ] Example apps
- [ ] CI/CD (GitHub Actions with Android emulator)

### Phase 4 — Advanced Features
- [ ] pg_trgm extension (fuzzy search)
- [ ] Database encryption at rest
- [ ] Backup/restore to file
- [ ] Database size monitoring / auto-vacuum
- [ ] x86 architecture support (Chromebooks)
