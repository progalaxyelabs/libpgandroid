# pgAndroid Build Notes

Cross-compilation of PostgreSQL 17.5 (pglite base) for Android (aarch64).

**Output**: `out/arm64-v8a/libpgandroid.so` — 17 MB, ELF 64-bit LSB shared object, ARM aarch64, for Android 34.
**Toolchain**: Android NDK r27c (27.2.12479018), `aarch64-linux-android34-clang`.

---

## Build Command

```bash
./build.sh --arch arm64-v8a --jobs $(nproc)
```

See `build.sh --help` for options.

---

## Prerequisites

| Requirement | Path |
|---|---|
| Android NDK | `$ANDROID_NDK_HOME` (set in `env.sh`) |
| OpenSSL (arm64) | `deps/openssl-android-arm64/` |
| PostgreSQL source | `upstream/postgres-pglite/` (REL\_17\_5\_WASM branch) |

---

## Fixes Applied During Initial Build

The following issues were resolved while establishing the Android cross-compilation path. They are documented here to explain why the files in `build/pg-src/` differ from the upstream pglite source.

### 1. PGANDROID\_DIR wrong path

**Error**: `No rule to make target '../pgandroid/pgandroid/pgandroid_main.c'`

**Cause**: `make` was invoked without the `PGANDROID_DIR` variable; the default in the old `Makefile.android` was `../pgandroid/pgandroid`.

**Fix**: Added `PGANDROID_DIR="$PG_SRC/pgandroid"` to make invocations in `build.sh`.

---

### 2. Makefile.android was a standalone build system

**Error**: `No rule to make target 'src/backend/libpgcore.a', needed by 'libpgandroid.so'`

**Cause**: `src/makefiles/Makefile.android` was included as `src/Makefile.port` (the standard PostgreSQL port makefile mechanism). It contained a full `all: libpgandroid.so` target which added `libpgandroid.so` to the top-level `all` target, causing it to try to build before `libpgcore.a` existed. The emscripten/wasi ports use MINIMAL `Makefile.port` files (just `rpath`, `AROPT`, and a `.so` pattern rule).

**Fix**:
- Replaced `src/makefiles/Makefile.android` with a minimal port file (same pattern as emscripten/wasi).
- Added `android` case to `src/backend/Makefile` to build `libpgcore.a` (same approach as wasi):
  - Excluded `android` from the regular `postgres` executable target.
  - Added `ifeq ($(PORTNAME), android)` block that produces `libpgcore.a` via a partial-link step.

---

### 3. `android_pgandroid.h` not found (symlink-relative include)

**Error**: `'android_pgandroid.h' file not found` (from `pg_config_os.h:18`)

**Cause**: `pg_config_os.h` is a symlink at `src/include/pg_config_os.h` → `src/include/port/android.h`. Clang resolves `#include "..."` relative to the **symlink path** (`src/include/`), not the target path (`src/include/port/`). So `#include "android_pgandroid.h"` failed to find `src/include/port/android_pgandroid.h`.

**Fix**: Changed `#include "android_pgandroid.h"` to `#include <port/android_pgandroid.h>` (angle brackets use the compiler's include search path, where `src/include` is listed).

---

### 4. Missing `#endif` in `android.h`

**Error**: `error: unterminated conditional directive` (at `pg_config_os.h:10`, which is `android.h`)

**Cause**: `src/include/port/android.h` was missing its closing `#endif /* I_ANDROID_PGANDROID */`.

**Fix**: Added `#endif /* I_ANDROID_PGANDROID */` at end of `android.h`.

---

### 5. `syncfs` / `sync_file_range` undeclared

**Error**: `error: implicit declaration of function 'syncfs'`

**Cause**: These Linux syscalls are guarded by `__USE_GNU` in NDK headers. `_GNU_SOURCE` was defined via `template_defines` in `src/template/android`, which is not processed the same way as `CPPFLAGS`.

**Fix**:
- Added `-D_GNU_SOURCE` to `CPPFLAGS` in `build.sh`.
- Updated `src/template/android` to use `CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"` (matching the emscripten template pattern).
- Deleted `GNUmakefile` to force `configure` re-run.

---

### 6. Function-like macro breaks declaration in `fd.h`

**Error**: `error: type specifier missing` / `error: conflicting types for 'android_OpenPipeStream'`

**Cause**: `android_pgandroid.h` defined `#define OpenPipeStream(cmd, mode) android_OpenPipeStream((cmd), (mode))`. When the C preprocessor expanded this against the declaration `extern FILE *OpenPipeStream(const char *command, const char *mode)` in `fd.h`, it produced the invalid form `android_OpenPipeStream((const char *command), (const char *mode))` — parameter type declarations wrapped in extra parentheses are not valid C.

**Fix**: Removed the `OpenPipeStream` macro from `android_pgandroid.h`.

---

### 7. Conflicting types for `BootstrapModeMain`

**Error**: `error: conflicting types for 'BootstrapModeMain'`

**Cause**: `bootstrap.c` defines `BootstrapModeMain` as returning `int` when `__ANDROID_PGANDROID__` is set, but `bootstrap.h` only had the `int` version guarded for `__EMSCRIPTEN__ || __wasi__`.

**Fix**: Added `|| defined(__ANDROID_PGANDROID__)` to the condition in `bootstrap.h`.

---

### 8. `static` vs non-`static` conflict in `be-fsstubs.c`

**Error**: `error: static declaration of 'lo_read3' follows non-static declaration`

**Cause**: `be-fsstubs.c` made `lo_read3`/`lo_write3` `static` when `__ANDROID_PGANDROID__` was defined, but `be-fsstubs.h` declares them as `extern`.

**Fix**: Removed `__ANDROID_PGANDROID__` from the static guard in `be-fsstubs.c` (Android gets the same `int`/non-static definition as the default path).

---

### 9. `pgandroid_io_read`/`write` undeclared + wrong WASM code path in `pqcomm.c`

**Error (a)**: `error: implicit declaration of function 'pgandroid_io_read'`
**Error (b)**: `error: use of undeclared identifier 'CMA_MB'` (Android was incorrectly included in the WASM code path)

**Cause (a)**: `android_pgandroid.h` declared `pgandroid_io_put_input`, `pgandroid_io_get_output`, and `pgandroid_io_reset` but not the lower-level `pgandroid_io_read`/`pgandroid_io_write` functions called by the patched `pqcomm.c`.

**Cause (b)**: The patched `pqcomm.c` `internal_putbytes` was guarded by `#if defined(__EMSCRIPTEN__) || defined(__wasi__) || defined(__ANDROID_PGANDROID__)`, mixing Android with WASM-specific variables (`CMA_MB`, `cma_rsize`, `SOCKET_FILE`, `SOCKET_DATA`) that don't exist on Android.

**Fix (a)**: Added `pgandroid_io_read`/`pgandroid_io_write` declarations to `android_pgandroid.h`.

**Fix (b)**: Split the `pqcomm.c` patch: WASM path uses `#if defined(__EMSCRIPTEN__) || defined(__wasi__)`, Android path uses `#elif defined(__ANDROID_PGANDROID__)` with a clean buffer-based implementation.

---

### 10. ICU functions undeclared

**Error**: `error: implicit declaration of function 'u_isdigit'`

**Cause**: `configure` detected ICU on the build host, but ICU headers are not available for Android cross-compilation.

**Fix**: Added `--without-icu` to the `configure` invocation in `build.sh`. Deleted `GNUmakefile` to force reconfigure.

---

### 11. `shm_open`/`shm_unlink` undeclared

**Error**: `error: implicit declaration of function 'shm_open'`

**Cause**: Android bionic does not expose `shm_open`/`shm_unlink` in NDK headers. `HAVE_SHM_OPEN` was unconditionally set to `1` in `src/include/port.h` inside `#ifndef WIN32`.

**Fix**: Added `#ifndef __ANDROID_PGANDROID__` guard around `#define HAVE_SHM_OPEN 1` in `port.h`.

---

### 12. Top-level `make` builds ecpg (not needed)

**Error**: `make[4]: *** [Makefile:42: submake-ecpglib] Error 2`

**Cause**: `make -C "$PG_SRC"` builds all of `src/`, including `src/interfaces/ecpg` (Embedded SQL for C), which fails to cross-compile and is not needed for the Android backend.

**Note**: `libpgcore.a` (15 MB) was already built successfully before this error.

**Fix**: Changed `build.sh` to run `make -C "$PG_SRC/src/backend"` instead of the top-level `make -C "$PG_SRC"`. This directly targets the backend Makefile (which has the android case producing `libpgcore.a`) without triggering ecpg/interfaces.

---

### 13. `Makefile.shlib` has no `android` PORTNAME case

**Error**: `make: o: No such file or directory` (when building pgcrypto extension)

**Cause**: `src/Makefile.shlib` defines `LINK.shared` for known port names (linux, darwin, openbsd, etc.) but not for `android`. With `PORTNAME=android`, `LINK.shared` was empty. The link command `$(LINK.shared) -o pgcrypto.so $(OBJS)...` degraded to `-o pgcrypto.so ...` which the shell tried to execute as command `o`.

**Fix**: Added `ifeq ($(PORTNAME), android)` block to `src/Makefile.shlib` setting `LINK.shared = $(COMPILER) -shared` (same as linux, with Android's unversioned soname convention).

---

### 14. `uuid-ossp` requires external UUID library

**Error**: `#error "please use configure's --with-uuid switch to select a UUID library"`

**Cause**: The `uuid-ossp` extension requires either `ossp-uuid` or `e2fsprogs libuuid`. Neither is available in the Android NDK sysroot.

**Resolution**: `uuid-ossp` is **skipped** in `build.sh`. PostgreSQL 17 provides `gen_random_uuid()` natively (using OpenSSL's `RAND_bytes`), which covers all embedded use cases.

---

## Outputs

| File | Description |
|---|---|
| `out/arm64-v8a/libpgandroid.so` | 17 MB stripped ARM64 shared library |
| `out/arm64-v8a/libpgandroid.h` | Public C API header |

### Public API (`libpgandroid.h`)

```c
/* Initialize PostgreSQL (runs initdb on first call). Returns 0 on success. */
int   pgandroid_init(const char *datadir);

/* Execute SQL. Returns malloc'd JSON string (caller must free). */
char *pgandroid_exec(const char *sql);

/* Execute SQL with JSON parameter array. Returns malloc'd JSON string. */
char *pgandroid_exec_params(const char *sql, const char *params_json);

/* Graceful shutdown (runs PG exit hooks, then returns). */
void  pgandroid_close(void);
```

---

## Known Limitations

- **uuid-ossp**: Not built (no libuuid in NDK). Use `gen_random_uuid()` instead.
- **pgandroid_exec_params**: Currently delegates to `pgandroid_exec` (extended-query Bind/Execute support is a future enhancement).
- **Single-threaded**: All calls must be serialised from the caller side (e.g. Kotlin `Mutex`).
- **No WAL archiving or replication**: pgandroid runs in single-user embedded mode only.
