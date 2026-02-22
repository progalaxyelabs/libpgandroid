# PGlite Patch Analysis: REL_17_5 → REL_17_5_WASM

**Generated:** 2026-02-22
**Source:** https://github.com/electric-sql/postgres-pglite
**Base branch:** `REL_17_5` (upstream PostgreSQL 17.5)
**Target branch:** `REL_17_5_WASM` (PGlite WASM port)
**Note on `pglite-wasm/`:** The custom entry-point directory lives in the **`REL_17_5_WASM-pglite`** branch (on top of `REL_17_5_WASM`), not the `REL_17_5_WASM` branch itself. Both branches are documented here.
**Diff file:** `docs/pglite-patches-full.diff` (3878 lines, 76 files)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Patch Classification Key](#2-patch-classification-key)
3. [Preprocessor Guards Reference](#3-preprocessor-guards-reference)
4. [Category-by-Category File Analysis](#4-category-by-category-file-analysis)
5. [pglite-wasm/ Entry Point Analysis](#5-pglite-wasm-entry-point-analysis)
6. [Android Port Strategy](#6-android-port-strategy)
7. [Files Needing Android-Specific Alternatives](#7-files-needing-android-specific-alternatives)
8. [Where to Add `__ANDROID_PGANDROID__` Guards](#8-where-to-add-__android_pgandroid__-guards)

---

## 1. Executive Summary

The PGlite WASM port makes PostgreSQL run in a **single-process, no-fork, no-signals** environment inside a WebAssembly sandbox. The changes fall into three broad buckets:

| Category | Count | Description |
|----------|-------|-------------|
| **WASM_SPECIFIC** | ~30 files | Emscripten/JS interop, WASM memory model, WASI syscall stubs |
| **GENERIC_SINGLE_PROCESS** | ~35 files | Remove forking, IPC, signals — reusable for Android |
| **BUILD/DEBUG** | ~11 files | Makefiles, templates, debug `puts()` instrumentation |

**For Android (pgandroid):** The generic single-process patches are directly applicable (with minor guards changed from `__EMSCRIPTEN__`/`__wasi__` to `__ANDROID_PGANDROID__`). The WASM-specific patches need Android-specific rewrites, particularly:
- Socket I/O layer → JNI direct calls or Unix domain sockets
- `proc_exit` handling → JNI exception or longjmp
- Bootstrap/main entry → JNI exported C function

---

## 2. Patch Classification Key

| Tag | Meaning |
|-----|---------|
| `WASM_SPECIFIC` | Uses Emscripten JS interop (`EM_JS`, `EMSCRIPTEN_KEEPALIVE`), WASI syscall overrides, or WebAssembly memory model specifics. **Not directly portable to Android.** |
| `GENERIC_SINGLE_PROCESS` | Removes multi-process features (fork, SysV IPC, signals, parallel workers). Applicable to any embedded/single-process port. **Reuse for Android with guard change.** |
| `ANDROID_NEEDS_ALTERNATIVE` | The WASM approach is wrong for Android but Android still needs something here (e.g., different socket layer, different logging). |
| `DISABLED_FOR_WASM` | Feature completely stubbed or disabled. Android may want to re-enable. |
| `DEBUG_ENHANCEMENT` | Debug `puts()` / `fprintf(stderr)` instrumentation. Keep or remove. |
| `BUILD_CHANGE` | Makefile / configure changes. Android will need separate build system. |

---

## 3. Preprocessor Guards Reference

### Guards used in `REL_17_5_WASM` patches

| Guard | Where used | Meaning |
|-------|-----------|---------|
| `__EMSCRIPTEN__` | Most backend files | Emscripten compiler (WASM via JS) |
| `__wasi__` | Most backend files | WebAssembly System Interface (WASI) |
| `I_WASM` | emscripten.h / wasi.h | Internal: "we are in a WASM build" |
| `I_EMSCRIPTEN` | pqcomm.c | Emscripten-specific socket emulation active |
| `I_WASI` | pqcomm.c | WASI-specific socket emulation active |
| `PGL_MAIN` | postgres.c, logging.c | Building pglite combined main |
| `PG_FD` | fd.c, wasm_common.h | Building fd.c (activates pipe stubs) |
| `PG_SHMEM` | sysv_shmem.c, wasm_common.h | Building shmem (activates malloc stubs) |
| `PG_POSTINIT` | postinit.c | Building postinit.c (debug markers) |
| `PG_EXEC` | exec.c | Building exec.c |
| `FE_UTILS_PRINT` | fe_utils/print.c | Building print utils |
| `PGL_INITDB_MAIN` | pgl_initdb.c | Building initdb path |

### Guards to add for Android

| Guard | Recommended meaning |
|-------|-------------------|
| `__ANDROID_PGANDROID__` | "Building pgandroid single-process embedded PostgreSQL for Android" |
| `PGANDROID_JNI` | "JNI layer is active — replace socket I/O with direct function calls" |

---

## 4. Category-by-Category File Analysis

### 4.1 IPC / Semaphores / Shared Memory

#### `src/backend/port/posix_sema.c` — GENERIC_SINGLE_PROCESS

**What it does:** POSIX semaphore management. `PGSemaphoreReset()` loops `sem_trywait()` to drain a semaphore.

**Patch:** Wraps the draining loop in `#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)`. For WASM: calls `sem_trywait` once and returns immediately.

**Rationale:** In single-process mode there are no other processes that could have posted to a semaphore; the loop is unnecessary and can deadlock.

**Android:** Reuse. Change guard to `__ANDROID_PGANDROID__`.

---

#### `src/backend/port/sysv_shmem.c` — GENERIC_SINGLE_PROCESS

**What it does:** `PGSharedMemoryCreate()` — allocates PostgreSQL's shared memory segment using SysV IPC (`shmget`/`shmat`).

**Patch:** The SysV IPC implementation is wrapped/bypassed. The stub in `wasm_common.h` (under `PG_SHMEM`) replaces `shmget`/`shmat`/`shmdt`/`shmctl` with:
- `shmget` → `malloc()` (stores pointer as `FAKE_SHM`, returns fake key `666`)
- `shmat` → returns `FAKE_SHM` pointer
- `shmdt` / `shmctl` → no-ops

**Rationale:** No kernel IPC in WASM sandbox. Single process owns all memory so shared memory can be a plain heap allocation.

**Android:** Reuse the pattern. Android does support SysV IPC but for embedded use, `malloc()` is simpler and avoids kernel resource leaks. Change to `__ANDROID_PGANDROID__`.

---

#### `src/include/storage/ipc.h` — WASM_SPECIFIC

**What it does:** Declares `proc_exit()`.

**Patch:** For WASM, renames `proc_exit` → `pg_proc_exit` (via `#define proc_exit(arg) pg_proc_exit(arg)` in `wasm_common.h`) and removes the `pg_attribute_noreturn()` annotation since the WASM version must return control.

**Rationale:** In WASM, calling `exit()` terminates the tab. PGlite needs to be able to "exit" and be called again without reloading. `pg_proc_exit` returns instead of calling `exit()`.

**Android:** ANDROID_NEEDS_ALTERNATIVE. Android also cannot call `exit()` from a JNI library without crashing the app. The pgandroid port needs a similar mechanism: `pg_proc_exit` returns to JNI, which translates to a Java exception or error code. Use `__ANDROID_PGANDROID__` guard with the same `#define proc_exit(arg) pg_proc_exit(arg)` approach.

---

#### `src/backend/storage/ipc/ipc.c` — WASM_SPECIFIC (partially GENERIC)

**What it does:** `proc_exit()` — runs exit callbacks (on_proc_exit list) then calls `exit()`.

**Patch:** For WASM, adds special case for exit code `66` (fake shutdown signal). Instead of calling `exit()`, the WASM version runs cleanup and then **returns** to the caller. This allows JS to call the PostgreSQL entry point again.

**Rationale:** Code 66 = "graceful WASM shutdown, run cleanup but do not actually exit". Exported as `pgl_shutdown()` in `pgl_mains.c`.

**Android:** ANDROID_NEEDS_ALTERNATIVE. Use the same "return instead of exit" trick. May want a different sentinel code or a flag to communicate the shutdown reason to JNI. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/storage/ipc/latch.c` — WASM_SPECIFIC

**What it does:** `InitializeLatchSupport()` — sets up self-pipe or eventfd for latch wakeups (used by background workers and postmaster).

**Patch:** Entire body wrapped in `#if !defined(__wasi__)`. For WASM: no-op.

**Rationale:** No background processes, no `epoll`/`select` between processes.

**Android:** GENERIC_SINGLE_PROCESS for Android too. Same no-op is appropriate. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/storage/ipc/procsignal.c` — GENERIC_SINGLE_PROCESS

**What it does:** `WaitForProcSignalBarrier()` — waits for all backends to acknowledge a signal barrier.

**Patch:** Adds `break` inside the wait loop under WASM guard. The loop breaks immediately since there are no other backends.

**Android:** Reuse. Change guard to `__ANDROID_PGANDROID__`.

---

#### `src/backend/storage/ipc/signalfuncs.c` — WASM_SPECIFIC

**What it does:** `pg_signal_backend()` — sends signals to backend processes via `kill()`.

**Patch:** Replaces `kill()` call with a no-op stub for WASM, with debug output.

**Android:** GENERIC_SINGLE_PROCESS for Android. Android doesn't use multiple processes for database backends. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/storage/ipc/sinvaladt.c` — GENERIC_SINGLE_PROCESS

**What it does:** `SharedInvalBackendInit()` — registers backend in shared invalidation message array.

**Patch:** For WASM: downgrades ERROR to WARNING when a backend slot is already in use. In single-process mode, the slot is always "reused" since there's only one process.

**Android:** Reuse. Change guard to `__ANDROID_PGANDROID__`.

---

#### `src/include/storage/dsm_impl.h` — WASM_SPECIFIC

**What it does:** Selects the DSM (dynamic shared memory) implementation.

**Patch:** Forces `DSM_IMPL_POSIX` for WASM, bypassing the `WIN32`/`HAVE_SHM_OPEN` autoconf selection.

**Android:** Likely reuse — Android supports POSIX shared memory. If embedding in a single process, DSM may not be needed at all.

---

### 4.2 Process / Signals

#### `src/backend/postmaster/postmaster.c` — DEBUG_ENHANCEMENT

**What it does:** Main postmaster process — forks backend processes, manages lifecycle.

**Patch:** Only adds `"%s:1536:"` line number to one error message. No functional change.

**Android:** No change needed.

---

#### `src/backend/postmaster/checkpointer.c` — GENERIC_SINGLE_PROCESS

**What it does:** `RequestCheckpoint()` — called from non-checkpointer processes. Checks if caller is a standalone backend.

**Patch:** Wraps the standalone-backend-only code path check in `#if !defined(__wasi__) && !defined(__EMSCRIPTEN__)`. For WASM: always runs the standalone backend path since there's no checkpointer process.

**Android:** Reuse. Change guard to `__ANDROID_PGANDROID__`.

---

#### `src/backend/utils/init/miscinit.c` — GENERIC_SINGLE_PROCESS

**What it does:** `InitPostmasterChild()` calls `setsid()`. `checkDataDir()` checks file ownership/permissions.

**Patch:**
- Wraps `setsid()` call in `#if defined(HAVE_SETSID) && !defined(__wasi__)`
- Wraps Unix permission checks in `#if !defined(WIN32) && !defined(__CYGWIN__) && !defined(__EMSCRIPTEN__) && !defined(__wasi__)`
- `CreateLockFile()` has special WASI path

**Android:** GENERIC_SINGLE_PROCESS. No session leader creation needed. File permission checks may be relevant on Android (DAC/MAC applies) — review individually. Guard non-applicable parts with `__ANDROID_PGANDROID__`.

---

#### `src/backend/utils/init/postinit.c` — DEBUG_ENHANCEMENT + GENERIC_SINGLE_PROCESS

**What it does:** `InitPostgres()` — main backend initialization sequence.

**Patch:** Adds `#define PG_POSTINIT` marker, inserts debug `puts()` calls at key init points, handles single-user-mode username with special WASM_USERNAME fallback.

**Android:** The debug markers can be removed. The `WASM_USERNAME` fallback for `--single` mode is relevant — Android similarly may need a hardcoded default username. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/tcop/postgres.c` — WASM_SPECIFIC

**What it does:** Main query-processing loop (`PostgresMain`, `PostgresSingleUserMain`).

**Patch:** Adds WASM-specific global variables:
- `volatile int cma_rsize` — cross-memory attachment receive buffer size
- `volatile bool sockfiles` — whether socket is file-emulated
- `bool quote_all_identifiers` — moved here from ruleutils.c
- `FILE* SOCKET_FILE`, `int SOCKET_DATA` — socket file emulation
Wraps `PostgresSingleUserMain` in `#if !defined(PGL_MAIN)` since PGlite provides its own main.

**Android:** ANDROID_NEEDS_ALTERNATIVE. JNI port will provide its own entry point (`pgandroid_exec_query()` or similar). The `PostgresSingleUserMain` function is the right starting point but needs JNI-specific I/O binding instead of socket file emulation.

---

#### `src/backend/storage/lmgr/proc.c` — GENERIC_SINGLE_PROCESS

**What it does:** `InitProcess()` — initializes the backend's PGPROC entry.

**Patch:** Downgrades ERROR to WARNING when `MyProc` is already initialized (re-initialization in single-process mode is expected).

**Android:** Reuse. Change guard to `__ANDROID_PGANDROID__`.

---

#### `src/port/pthread_barrier_wait.c` — GENERIC_SINGLE_PROCESS

**What it does:** POSIX `pthread_barrier_wait()` implementation.

**Patch:** Wraps POSIX implementation in `#if !defined(__wasi__)`. For WASM: all barrier functions are no-ops returning 0.

**Android:** Android bionic does support pthread barriers, so this may not be needed. Verify and guard with `__ANDROID_PGANDROID__` if stubbing.

---

#### `src/interfaces/libpq/legacy-pqsignal.c` — WASM_SPECIFIC

**What it does:** `pqsignal()` — safe wrapper around `signal()` or `sigaction()`.

**Patch:** Entire function wrapped in `#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)`.

**Android:** GENERIC_SINGLE_PROCESS. Signal handling is available on Android via bionic, but may not be needed in single-process embedded mode. Guard with `__ANDROID_PGANDROID__` and keep if needed.

---

#### `src/port/pqsignal.c` — FORMATTING_ONLY

Minor whitespace change. No action needed.

---

#### `src/port/getpeereid.c` — WASM_SPECIFIC

**What it does:** `getpeereid()` — gets Unix socket peer credentials.

**Patch:** For WASM: immediately sets `errno = ENOSYS` and returns -1.

**Android:** GENERIC_SINGLE_PROCESS. In embedded mode with JNI the client and server are the same process; peer credentials are irrelevant. Same stub works. Guard with `__ANDROID_PGANDROID__`.

---

### 4.3 Transaction / WAL

#### `src/backend/access/transam/xact.c` — GENERIC_SINGLE_PROCESS

**What it does:** Transaction management — `AbortTransaction`, `AbortSubTransaction`, etc.

**Patch:**
- Adds `elog(WARNING)` about aborting transaction
- Wraps `sigprocmask()` calls (for restoring signal masks) in `#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)`

**Rationale:** No signal mask manipulation in WASM (no signals). Transaction abort is still fully functional.

**Android:** GENERIC_SINGLE_PROCESS. `sigprocmask` is available on Android but if running embedded (no signal handling), the guard is correct. Change to `__ANDROID_PGANDROID__`.

---

#### `src/backend/access/transam/xlogarchive.c` — WASM_SPECIFIC

**What it does:** WAL archiving — uses `system()` to run archive commands.

**Patch:** For WASI: `#define system(cmd) system_wasi(cmd)` — routes to a WASI-specific `system()` implementation.

**Android:** Android has `system()` via bionic. WAL archiving is likely not needed in embedded mode. Can stub or disable entirely. Guard with `__ANDROID_PGANDROID__`.

---

### 4.4 File I/O

#### `src/backend/storage/file/fd.c` — GENERIC_SINGLE_PROCESS + WASM_SPECIFIC

**What it does:** Virtual file descriptor management, `pg_flush_data()`, `OpenPipeStream()`/`ClosePipeStream()`.

**Patch:**
- Adds `#define PG_FD` marker at top (triggers `wasm_OpenPipeStream` stub in `wasm_common.h`)
- `pg_flush_data()`: uses simple `fsync()` instead of `sync_file_range()` for WASM
- `OpenPipeStream()`/`ClosePipeStream()`: wrapped in `#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)`. For WASM: `OpenPipeStream` is redirected to `wasm_OpenPipeStream()` which creates a fake locale file instead of running `locale -a`

**Android:** ANDROID_NEEDS_ALTERNATIVE for pipe streams. Android may need to intercept `locale -a` similarly (no `locale` binary). `pg_flush_data` change is GENERIC_SINGLE_PROCESS and reusable. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/libpq/be-fsstubs.c` — GENERIC_SINGLE_PROCESS

**What it does:** Large object backend stubs — `lo_read()` and `lo_write()`.

**Patch:** Renames `lo_read` → `lo_read3`, `lo_write` → `lo_write3`, makes them `static` and wraps in WASM guard.

**Android:** Reuse. The rename avoids symbol conflicts in a combined link unit. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/include/storage/fd.h` — GENERIC_SINGLE_PROCESS

**What it does:** Declares `fsync_fname()` and `durable_rename()`.

**Patch:** Renames to `fd_fsync_fname` / `fd_durable_rename` and adds `#define` macros for compatibility. This avoids conflicts with identical declarations in `src/include/common/file_utils.h`.

**Android:** Reuse. Same naming conflict exists in any combined build. Guard with `__ANDROID_PGANDROID__`.

---

### 4.5 Network / Protocol

#### `src/backend/libpq/pqcomm.c` — WASM_SPECIFIC (most critical file)

**What it does:** All client-server socket communication — `pq_init()`, `pq_recvbuf()`, `internal_putbytes()`, `internal_flush()`.

**Patch:** The most heavily modified file. The WASM port replaces socket I/O with:
1. **File-based socket emulation** (`SOCKET_FILE`, `PGS_IN`/`PGS_OUT` paths in `wasm_common.h`) — reads PostgreSQL wire-protocol messages from files instead of a TCP/Unix socket
2. **CMA (cross-memory attachment) buffer** — a shared memory region (`cma_rsize`) for passing data between JS and the WASM module
3. `pq_recvbuf_fill()` exported as `EMSCRIPTEN_KEEPALIVE` — called from JavaScript to push data into the receive buffer
4. `pq_startmsgread()` — switches between file-mode and CMA-mode input
5. `internal_flush()` — writes responses to CMA buffer or `SOCKET_FILE` instead of socket

**Android:** ANDROID_NEEDS_ALTERNATIVE — this is the most critical rewrite target.
- For pgandroid JNI: replace with direct function calls (no socket needed)
- `pq_recvbuf_fill()` becomes a JNI-callable C function that puts a pre-built wire protocol message into the buffer
- `internal_flush()` collects the response and makes it available to JNI
- Alternative: keep Unix domain sockets and let JNI connect as a libpq client — simpler but adds latency

---

#### `src/backend/libpq/auth.c` — GENERIC_SINGLE_PROCESS

**What it does:** `auth_peer()` — peer authentication using `getpeereid()`.

**Patch:** Wraps `getpwuid()` in `#if !defined(WIN32) && !defined(__wasi__)`. For WASM: skips credential verification.

**Android:** GENERIC_SINGLE_PROCESS. In embedded mode, auth is internal — no network client. Skip peer auth. Guard with `__ANDROID_PGANDROID__`.

---

### 4.6 Utility / GUC / Misc

#### `src/backend/utils/misc/timeout.c` — GENERIC_SINGLE_PROCESS

**What it does:** `insert_timeout()` and `schedule_alarm()` — SIGALRM-based timeout mechanism.

**Patch:**
- `insert_timeout()`: for WASM, logs a warning and returns early on first call
- `schedule_alarm()`: wraps `setitimer()` in `#if !defined(__wasi__)`

**Rationale:** No `SIGALRM` in WASM. Timeouts are effectively disabled.

**Android:** GENERIC_SINGLE_PROCESS. Android bionic supports `SIGALRM` and `setitimer`, so this may not be needed. If running without signals (embedded mode), guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/utils/misc/guc.c` — GENERIC_SINGLE_PROCESS

**What it does:** GUC (Grand Unified Configuration) system. `SelectConfigFiles()` calls `ProcessConfigFile()`.

**Patch:** Removes both `ProcessConfigFile()` calls — config file reading is disabled in WASM.

**Android:** ANDROID_NEEDS_ALTERNATIVE. Android may want to keep config file reading (from app-internal storage) or pass options programmatically. If using `--single` mode options array, config file reading may be safely disabled. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/utils/fmgr/dfmgr.c` — WASM_SPECIFIC

**What it does:** Dynamic library function lookup via `dlsym()`.

**Patch:** Wraps error reporting in `#if !defined(__wasi__)`. For WASI: prints to `stderr` instead of using `ereport`.

**Android:** Android supports `dlopen`/`dlsym`. No change needed here.

---

#### `src/backend/bootstrap/bootstrap.c` — WASM_SPECIFIC

**What it does:** `BootstrapModeMain()` — runs the bootstrap backend that initializes the catalog.

**Patch:** Changes return type from `void` to `int` and replaces `proc_exit()` with `puts()` + return. The function must return to allow the PGlite JavaScript to continue initialization.

**Android:** WASM_SPECIFIC but similar constraint applies to JNI. `BootstrapModeMain` should return to JNI rather than calling exit. Guard with `__ANDROID_PGANDROID__` to change return type. Then call `pg_proc_exit()` (which returns) instead of `proc_exit()`.

---

#### `src/backend/commands/dbcommands.c` — GENERIC_SINGLE_PROCESS

**What it does:** `dropdb()` sends a `ProcSignalBarrier` after dropping a database.

**Patch:** Wraps `WaitForProcSignalBarrier()` in WASM guard. No other backends to signal.

**Android:** Reuse. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/commands/event_trigger.c` — GENERIC_SINGLE_PROCESS

**What it does:** DDL event trigger dispatch — checks `IsUnderPostmaster` before firing triggers.

**Patch:** Removes `IsUnderPostmaster` check for WASM. Always processes event triggers in single-user mode. Applied to: `EventTriggerDDLCommandStart`, `EventTriggerDDLCommandEnd`, `EventTriggerSQLDrop`, `EventTriggerOnLogin`, `EventTriggerTableRewrite`.

**Android:** Reuse. Single-process Android always runs as standalone backend, same as WASM. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/tcop/utility.c` — GENERIC_SINGLE_PROCESS

**What it does:** `LISTEN` command — checks if running in a background process.

**Patch:** Removes the IsBackgroundWorker check for WASM.

**Android:** Reuse.

---

#### `src/backend/utils/adt/ruleutils.c` — GENERIC_SINGLE_PROCESS

**What it does:** `quote_all_identifiers` global variable definition.

**Patch:** Wraps in `#if !defined(__EMSCRIPTEN__) && !defined(__wasi__)`. For WASM, this variable is defined in `tcop/postgres.c` instead to avoid duplicate symbols in a combined link unit.

**Android:** Reuse. Same combined-link-unit issue applies. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/backend/access/nbtree/nbtutils.c` — DEBUG_OPTIMIZATION

**What it does:** B-tree deduplication debug logging.

**Patch:** Wraps debug `ereport` in `#if !defined(__EMSCRIPTEN__)` to reduce WASM output.

**Android:** Optional. Keep or remove debug output as needed.

---

#### `src/backend/catalog/index.c` — DEBUG_OPTIMIZATION

**What it does:** Parallel index build debug logging.

**Patch:** Wraps debug `ereport` in `#if !defined(__EMSCRIPTEN__)`.

**Android:** Optional.

---

#### `src/contrib/pgstattuple/pgstatindex.c` — RELAXED_ERROR_HANDLING

**What it does:** Hash index page corruption error.

**Patch:** Changes ERROR to WARNING, adds debug output.

**Android:** Review individually. Probably keep as ERROR on Android.

---

### 4.7 New Headers

#### `src/include/port/wasm_common.h` — WASM_SPECIFIC (central hub)

The most important new file. Contains:

| Feature | Description |
|---------|-------------|
| `WAIT_USE_POLL` | Forces poll-based waiting instead of event-based |
| `WASM_USERNAME` | Default `"postgres"` for `--single` mode |
| `PGS_IN`/`PGS_OUT` | Socket file emulation paths |
| `WASM_PGOPTS` | Startup option array for WASM (disables checkpoints, sets buffer sizes) |
| `wasm_OpenPipeStream()` | Intercepts `locale -a` → returns fake locale file |
| `shmget`/`shmat`/`shmdt`/`shmctl` | Malloc-based shared memory stubs (when `PG_SHMEM` defined) |
| `pg_char_to_encoding_private` | Encoding function proxies (avoids frontend/backend conflicts) |
| `#define proc_exit(arg) pg_proc_exit(arg)` | Renames proc_exit globally |
| `EMSCRIPTEN_KEEPALIVE` | Mark functions as JS-callable (no-op when not in Emscripten) |

**Android equivalent:** Create `src/include/port/android_pgandroid.h` with Android-specific versions:
- Replace socket file emulation with JNI buffer passing
- Replace `WASM_PGOPTS` with `ANDROID_PGOPTS`
- Keep the `proc_exit` → `pg_proc_exit` rename
- Provide a real `popen` implementation or stub

---

#### `src/include/port/emscripten.h` — WASM_SPECIFIC

**Contents:** Guards (`I_WASM`), includes `sdk_port.h` (Emscripten SDK), includes `wasm_common.h`, defines `BOOT_END_MARK` and `FD_BUFFER_MAX`.

**Android equivalent:** `src/include/port/android.h` — include `android_pgandroid.h`, set Android-specific defines.

---

#### `src/include/port/wasi.h` — WASM_SPECIFIC

Identical in structure to `emscripten.h`. Both are effectively the same WASM compatibility header.

---

#### `src/include/port/pg_pthread.h` — GENERIC_SINGLE_PROCESS

**Patch:** Wraps `pthread_barrier_t` typedef and barrier function declarations in `#if !defined(__wasi__)`. Provides empty stubs for WASI.

**Android:** Android bionic supports pthreads including barriers. Likely no change needed.

---

#### `src/include/utils/elog.h` — WASM_SPECIFIC

**What it does:** `ereport_domain` macro — uses `__builtin_constant_p` to optimize error level checks.

**Patch:** For WASM, uses simplified version of the macro without `pg_unreachable()`. The WASM compiler (`clang-wasm32`) handles some builtins differently.

**Android:** Likely not needed — Android NDK/clang handles `__builtin_constant_p` correctly.

---

#### `src/include/utils/palloc.h` — GENERIC_SINGLE_PROCESS

**Patch:** Wraps `extern CurrentMemoryContext` and `MemoryContextSwitchTo` in `#if !defined(PG_EXTERN)` guards to avoid duplicate declarations in combined build units.

**Android:** Reuse if doing combined link. Guard with `__ANDROID_PGANDROID__`.

---

#### `src/include/fmgr.h` — WASM_SPECIFIC

**Patch:** Undefines `PG_MODULE_MAGIC` for `__wasi__` and replaces with empty macro.

**Rationale:** Module magic number handling is different in WASI (no separate `.so` files).

**Android:** Android uses `.so` files. Keep `PG_MODULE_MAGIC`. No change needed.

---

### 4.8 Build System

#### `src/backend/Makefile` — BUILD_CHANGE

**Patch:** Adds Emscripten and WASI link targets:
- Emscripten: produces `libpgcore.a`, `libpostgres.a`, `libpgmain.a` using relocatable linking with `--whole-archive`
- WASI: produces `.wasm.wasi` with `-nostartfiles`

**Android:** Separate build system (CMake/ndk-build). Not applicable.

---

#### `src/makefiles/Makefile.emscripten` / `src/makefiles/Makefile.wasi` — BUILD_CHANGE

Standard make rules for WASM shared library compilation.

**Android:** Not applicable. Android uses CMake or ndk-build.

---

#### `src/template/emscripten` / `src/template/wasi` — BUILD_CHANGE

Autoconf templates: `PREFERRED_SEMAPHORES=UNNAMED_POSIX`, `-D_GNU_SOURCE`, `CFLAGS_SL="-fPIC"`.

**Android:** Use Android NDK toolchain file instead.

---

#### `contrib/xml2/Makefile` — BUILD_CHANGE

Adds `PG_CFLAGS=$(shell xml2-config --cflags)`. Likely fixing a build issue.

---

### 4.9 Frontend Tool Stubs

These files are modified primarily to allow the PGlite build to include frontend tools in a combined link unit (WASM compiles everything together). For Android, frontend tools (`pg_dump`, `pg_ctl`, etc.) are generally not included in the embedded library.

| File | Patch Summary | Android |
|------|--------------|---------|
| `src/bin/pg_config/pg_config.c` | Wrap `exit(1)` in WASM guard | Not applicable to embedded |
| `src/bin/pg_ctl/pg_ctl.c` | Undefine `HAVE_SETSID`/`HAVE_GETRLIMIT` for WASI, wrap `execl()` | Not applicable to embedded |
| `src/bin/pg_dump/parallel.c` | Stub `WaitForTerminatingWorkers` | Not applicable to embedded |
| `src/bin/pg_dump/pg_dump.c` | Rename `quote_all_identifiers`, wrap `chdir("/")` | Not applicable to embedded |
| `src/bin/pg_dump/pg_dumpall.c` | Same rename | Not applicable to embedded |
| `src/bin/pg_dump/pg_backup_db.c` | Debug `"#%d:%s"` prefix in errors | Not applicable to embedded |
| `src/bin/pg_resetwal/pg_resetwal.c` | Wrap root-user check | Not applicable to embedded |
| `src/bin/pg_upgrade/parallel.c` | Stub `reap_child()` | Not applicable to embedded |
| `src/bin/pg_verifybackup/pg_verifybackup.c` | Replace entire file with `return 0` stub | Not applicable to embedded |
| `src/bin/psql/command.c` | Stub `do_watch()` for WASI | Not applicable to embedded |

---

### 4.10 Common / Frontend Utils

| File | Patch | Android |
|------|-------|---------|
| `src/common/exec.c` | Add `#define PG_EXEC` marker | Keep marker if doing combined link |
| `src/common/logging.c` | Wrap `progname` extern in `#if !defined(PGL_MAIN)` | Reuse if combined link |
| `src/common/pg_get_line.c` | Wrap `sigsetjmp` in `#if !defined(__wasi__)` | Likely not needed on Android |
| `src/fe_utils/connect_utils.c` | Debug `puts()` at connection points | Remove |
| `src/fe_utils/print.c` | Add `#define FE_UTILS_PRINT` marker | Not applicable to embedded |
| `src/fe_utils/query_utils.c` | Add `"#%d:%s"` debug prefix | Remove |
| `src/fe_utils/string_utils.c` | Rename `quote_all_identifiers` → `fe_utils_quote_all_identifiers` | Reuse if combined link |
| `src/include/fe_utils/string_utils.h` | Conditional `libpq-fe.h` include path, rename extern | Reuse if combined link |
| `src/include/common/file_utils.h` | Guard function declarations against duplicates | Reuse if combined link |
| `src/include/common/logging.h` | Whitespace only | No action |
| `src/test/regress/GNUmakefile` | Skip regression tests for emscripten | Not applicable |
| `src/test/regress/pg_regress.c` | Undefine `HAVE_GETRLIMIT`, stub `execl`/`wait`/`raise` | Not applicable |
| `src/test/modules/libpq_pipeline/libpq_pipeline.c` | Add `"#%d:%s:"` debug prefix | Not applicable |

---

## 5. pglite-wasm/ Entry Point Analysis

> **Note:** These files are in the `REL_17_5_WASM-pglite` branch, not `REL_17_5_WASM`. They represent the JavaScript-facing API layer built on top of the WASM-patched PostgreSQL.

### File Map

```
pglite-wasm/
├── pg_main.c          ← Combined main: defines globals, includes initdb + postgres paths
├── pg_proto.c         ← PostgreSQL wire protocol handler (included into pg_main.c)
├── pgl_initdb.c       ← initdb integration: popen/pclose stubs, initdb.c include
├── pgl_mains.c        ← Exported JS entry points: interactive_file(), interactive_one()
├── pgl_os.h           ← popen/pclose override: routes to IDB_PIPE_BOOT / IDB_PIPE_SINGLE
├── pgl_sjlj.c         ← setjmp/longjmp error recovery for the query loop
├── pgl_stubs.h        ← PostgresMain() stub, memory alloc stubs, simple_prompt stub
├── pgl_tools.h        ← Utility functions: mkdirp, strconcat, setdefault
├── interactive_one.c  ← Single-query execution entry point for JS
├── pglite-modpython.c ← Python extension stub
├── native.sh          ← Native (non-WASM) build script for testing
├── build.sh           ← Emscripten build script
└── repl.html          ← Browser REPL demo page
```

---

### `pg_main.c` — The Combined Main

**What it does:** Single C translation unit that ties everything together. It:
1. Defines compile-time constants (`PGL_MAIN`, `PGL_INITDB_MAIN`, `REPL`)
2. Defines socket file emulation paths (`IDB_PIPE_BOOT`, `IDB_PIPE_SINGLE`)
3. Includes `pgl_os.h` (activates `popen`/`pclose` overrides)
4. Includes all needed PostgreSQL headers
5. Defines global state: `PREFIX`, `PGDATA`, `PGUSER`, `is_repl`, `is_node`, `is_embed`, `async_restart`
6. Defines status bitmask constants (`IDB_OK`, `IDB_FAILED`, etc.)
7. Includes `pgl_mains.c` (entry points)
8. Includes `pgl_initdb.c` (initdb runner)

**Key design choice:** Everything compiled as a single TU — no separate `initdb` binary. The initdb and postgres paths are co-linked and share memory.

**How to adapt for JNI:**
```c
// pgandroid_main.c
#define PGANDROID_MAIN
#define PGANDROID_INITDB_MAIN
#include "pgandroid_os.h"   // Android-specific popen/pclose, locale stubs
// ... include pgandroid_mains.c
// Export: JNIEXPORT void JNICALL Java_..._execQuery(JNIEnv*, ...)
```

---

### `pg_proto.c` — Wire Protocol Handler

**What it does:** This is not a standalone file — it is `#include`-d into `pgl_mains.c` as a code fragment. It handles the PostgreSQL frontend-backend wire protocol message dispatch (`switch(firstchar)`):
- `'Q'` (PqMsg_Query) → `exec_simple_query()`
- `'P'` (PqMsg_Parse) → `exec_parse_message()`
- `'B'` (PqMsg_Bind) → `exec_bind_message()`
- `'E'` (PqMsg_Execute) → `exec_execute_message()`
- ... and all other protocol message types

**How to adapt for JNI:**
- In JNI direct-call mode, skip the wire protocol entirely. Call `exec_simple_query()` directly from JNI.
- Keep this file only if implementing a libpq-compatible socket interface for Android (e.g., for compatibility with existing libpq clients).

---

### `pgl_os.h` — OS-Level Overrides

**What it does:** Overrides `popen()`/`pclose()` with `pgl_popen()`/`pgl_pclose()`. The `pgl_popen()` function:
1. On first call (boot stage): opens `IDB_PIPE_BOOT` (a temp file) for write — this captures initdb's bootstrap queries
2. On second call (single stage): opens `IDB_PIPE_SINGLE` for write — captures initdb's `--single` user queries
3. On subsequent calls: redirects to `stderr`

**Rationale:** initdb uses `popen()` to invoke `postgres --boot` and `postgres --single`. In WASM there's no subprocess, so popen is redirected to files that the WASM code reads back as input.

**How to adapt for Android:**
- Same trick works: intercept `popen("postgres --boot ...")` with a function that writes to a memory buffer
- Or: refactor initdb to call bootstrap directly (cleaner but more invasive)
- Create `pgandroid_os.h` with `popen` → `pgandroid_popen` override

---

### `pgl_stubs.h` — Stub Implementations

**What it does:** Provides stub/stub implementations needed for the combined link unit:
- `PostgresMain()` — empty stub (real entry is `PostgresSingleUserMain`)
- `startup_hacks()` — initializes spinlock
- `get_restricted_token()` — empty stub (Windows security, not needed in WASM)
- `pg_malloc()`, `pg_realloc()`, `pg_strdup()` — delegate to C stdlib
- `simple_prompt()` — returns empty string (no interactive prompting in WASM)
- `ProcessStartupPacket()` — stub returning `STATUS_OK`
- `select_default_timezone()` — reads `$TZ` environment variable

**How to adapt for Android:**
- Keep most stubs.
- `simple_prompt()` could show an Android dialog (unlikely needed for embedded DB).
- `pg_malloc`/`pg_free` stubs are always needed when combining frontend+backend.

---

### `pgl_sjlj.c` — setjmp/longjmp Error Recovery

**What it does:** Contains the `sigsetjmp`-based error recovery block for the query processing loop. When a PostgreSQL error occurs (`ereport(ERROR)`), PostgreSQL uses `longjmp` to unwind the stack. This fragment:
1. Sets up `local_sigjmp_buf` via `sigsetjmp`
2. On error (nonzero return from `sigsetjmp`): runs full error cleanup (abort transaction, reset state)
3. For WASI: skips `sigsetjmp` entirely (WASI doesn't reliably support it)

**How to adapt for Android:**
- Android bionic supports `sigsetjmp`. Use standard PostgreSQL error recovery.
- The WASM WASI stub (no sigsetjmp) is NOT appropriate for Android.
- Keep the standard error recovery path.

---

### `pgl_initdb.c` — initdb Integration

**What it does:** Included into `pg_main.c` to provide the `pgl_initdb_main()` function:
- Declares `pg_chmod()` as a no-op (WASM has no real file modes)
- Includes `common/logging.c` to avoid separate compilation
- Includes `bin/initdb/initdb.c` directly (single TU compilation)
- Provides locale override stubs

**How to adapt for Android:**
- Same single-TU approach works
- `pg_chmod()` stub may or may not be appropriate (Android has real chmod)
- Create `pgandroid_initdb.c` following the same pattern

---

## 6. Android Port Strategy

### Approach: Single-Process JNI Library

The recommended approach for pgandroid mirrors PGlite but replaces the WASM/JS layer with JNI:

```
libpgandroid.so
├── PostgreSQL backend (single-process patches)
├── initdb (single-TU compilation, same as pglite-wasm)
└── JNI entry points (pgandroid_jni.c)
    ├── pgandroid_initdb()       ← runs initdb in-process
    ├── pgandroid_start()        ← initializes backend
    ├── pgandroid_exec(query)    ← executes SQL, returns ResultSet
    └── pgandroid_close()        ← graceful shutdown (calls pg_proc_exit(66))
```

### Porting Phases

**Phase 1: Core single-process patches** (direct port from WASM patches)
- All GENERIC_SINGLE_PROCESS guards: change `__EMSCRIPTEN__`/`__wasi__` → `__ANDROID_PGANDROID__`
- `proc_exit` → `pg_proc_exit` rename (critical — cannot call `exit()` in a JNI lib)
- SysV shared memory → malloc (or keep SysV if acceptable on Android)
- Disable `SIGALRM` timers (Android signal handling in JNI threads is complex)

**Phase 2: I/O layer** (Android-specific rewrites)
- `pqcomm.c`: replace file/CMA emulation with JNI buffer passing
- `pgl_os.h` analog: intercept `popen` for initdb bootstrap
- Logging: route `ereport` output to Android logcat (`__android_log_print`)

**Phase 3: Entry points** (JNI-specific new code)
- `pgandroid_main.c`: JNI-exported functions replacing `pg_main.c`
- No `pg_proto.c` needed — call `exec_simple_query()` directly

**Phase 4: Build system**
- CMakeLists.txt with `add_library(pgandroid SHARED ...)`
- Android NDK toolchain
- `configure` for `aarch64-linux-android`

---

## 7. Files Needing Android-Specific Alternatives

| File | WASM Approach | Android Alternative |
|------|--------------|---------------------|
| `src/backend/libpq/pqcomm.c` | File/CMA socket emulation | JNI buffer passing OR Unix domain socket |
| `src/include/port/wasm_common.h` | JS interop, file socket paths | `android_pgandroid.h`: JNI buffer, logcat, Android paths |
| `src/include/port/emscripten.h` | Emscripten SDK include | `android.h`: NDK includes |
| `pglite-wasm/pg_main.c` | JS-facing combined main | `pgandroid_main.c`: JNI-facing combined main |
| `pglite-wasm/pgl_mains.c` | JS-exported entry points | JNI-exported entry points (`Java_...` functions) |
| `pglite-wasm/pg_proto.c` | Wire protocol for JS → WASM | Not needed (call `exec_simple_query` directly from JNI) |
| `src/backend/storage/ipc/ipc.c` | `proc_exit` returns (code 66) | Same pattern, JNI returns instead of calling exit |
| `src/backend/utils/misc/guc.c` | Config file reading disabled | Enable or pass config programmatically |

---

## 8. Where to Add `__ANDROID_PGANDROID__` Guards

These are the specific locations in PostgreSQL source where `__ANDROID_PGANDROID__` guards should be inserted (modeled on the WASM patches):

### Critical (must change for Android to work)

| File | Location | Change |
|------|----------|--------|
| `src/include/storage/ipc.h` | `proc_exit` declaration | Add `#define proc_exit(arg) pg_proc_exit(arg)` via included header |
| `src/backend/storage/ipc/ipc.c` | `proc_exit()` body | Return instead of calling `exit()` when `__ANDROID_PGANDROID__` |
| `src/backend/port/sysv_shmem.c` | `PGSharedMemoryCreate()` | Replace SysV with malloc |
| `src/backend/libpq/pqcomm.c` | Entire I/O layer | JNI buffer passing replaces socket |
| `src/backend/bootstrap/bootstrap.c` | `BootstrapModeMain` signature | Change to `int` return type |
| `src/include/bootstrap/bootstrap.h` | `BootstrapModeMain` declaration | Same |

### Important (single-process correctness)

| File | Location | Change |
|------|----------|--------|
| `src/backend/access/transam/xact.c` | `sigprocmask()` calls | Wrap in guard (skip signal mask restore) |
| `src/backend/storage/ipc/sinvaladt.c` | Slot conflict check | Downgrade ERROR to WARNING |
| `src/backend/storage/lmgr/proc.c` | `MyProc` already initialized | Downgrade ERROR to WARNING |
| `src/backend/postmaster/checkpointer.c` | Standalone backend check | Use ANDROID guard |
| `src/backend/commands/event_trigger.c` | `IsUnderPostmaster` checks | Remove check for Android |
| `src/backend/tcop/utility.c` | LISTEN background check | Remove for Android |
| `src/backend/storage/ipc/procsignal.c` | `WaitForProcSignalBarrier` | Break immediately |
| `src/backend/commands/dbcommands.c` | `WaitForProcSignalBarrier` after dropdb | Wrap in guard |

### Optional (stability / correctness)

| File | Location | Change |
|------|----------|--------|
| `src/backend/utils/misc/timeout.c` | `setitimer` calls | Evaluate — Android supports it |
| `src/backend/utils/misc/guc.c` | `ProcessConfigFile` calls | Decide: keep or disable |
| `src/backend/libpq/auth.c` | `getpwuid` in peer auth | Skip peer auth |
| `src/port/getpeereid.c` | Entire function | Return `ENOSYS` |
| `src/backend/storage/ipc/latch.c` | `InitializeLatchSupport` | No-op for embedded mode |
| `src/backend/utils/misc/miscinit.c` | `setsid`, file permission checks | Wrap in guard |
| `src/backend/storage/file/fd.c` | `OpenPipeStream` for `locale -a` | Intercept with Android locale stub |

---

*End of PATCH-ANALYSIS.md*
