/*
 * pgandroid/pgandroid_io.c
 *
 * I/O buffer layer for pgandroid. Replaces PGlite's file/CMA socket emulation
 * with a simple in-process byte buffer that the JNI layer fills (input) and
 * drains (output) without any socket or file I/O.
 *
 * Thread safety: pgandroid uses a persistent backend thread (started on the
 * first pgandroid_exec() call).  The backend thread blocks in pgandroid_io_read()
 * waiting for the next query.  The JNI/main thread supplies input via
 * pgandroid_io_put_input() and waits for the result via pgandroid_io_wait_result().
 *
 * Synchronization protocol:
 *   1. JNI thread: pgandroid_io_reset() — clear buffers
 *   2. JNI thread: pgandroid_io_put_input(wire, len) — supply query, signal in_cond
 *   3. Backend thread: pgandroid_io_read() wakes up, returns bytes
 *   4. Backend thread: processes query, writes response via pgandroid_io_write()
 *   5. Backend thread: pgandroid_io_read() called again, no input → signals out_cond, blocks
 *   6. JNI thread: pgandroid_io_wait_result() wakes up, returns
 *   7. JNI thread: pgandroid_io_get_output() collects response
 *
 * Wire protocol:
 *   Input  — a complete PostgreSQL frontend message (simple query: 'Q' + length + sql + \0)
 *   Output — the server's response messages, accumulated in pgandroid_out_buf
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Buffer sizes
 * ----------------------------------------------------------------------- */

#define PGANDROID_IN_BUF_INIT  (4 * 1024)    /* 4 KB initial capacity   */
#define PGANDROID_OUT_BUF_INIT (64 * 1024)   /* 64 KB initial capacity  */

/* -----------------------------------------------------------------------
 * Input buffer (query sent from JNI to PostgreSQL)
 * ----------------------------------------------------------------------- */

static char  *pgandroid_in_buf  = NULL;
static int    pgandroid_in_len  = 0;
static int    pgandroid_in_pos  = 0;   /* read cursor for pqcomm.c  */
static int    pgandroid_in_cap  = 0;

/* -----------------------------------------------------------------------
 * Output buffer (response accumulated from PostgreSQL)
 * ----------------------------------------------------------------------- */

static char  *pgandroid_out_buf = NULL;
static int    pgandroid_out_len = 0;
static int    pgandroid_out_cap = 0;

/* -----------------------------------------------------------------------
 * Thread synchronization
 * ----------------------------------------------------------------------- */

static pthread_mutex_t io_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  io_in_cond   = PTHREAD_COND_INITIALIZER; /* signals input ready */
static pthread_cond_t  io_out_cond  = PTHREAD_COND_INITIALIZER; /* signals output ready */

static volatile bool   io_in_ready  = false; /* new query is in the input buffer */
static volatile bool   io_out_ready = false; /* backend finished processing a query */
static volatile bool   io_closed    = false; /* shutdown requested */

/* -----------------------------------------------------------------------
 * Internal helpers (must be called with io_mutex held or during init)
 * ----------------------------------------------------------------------- */

static void
ensure_in_capacity(int needed)
{
    if (pgandroid_in_cap >= needed)
        return;
    pgandroid_in_cap = needed * 2;
    pgandroid_in_buf = realloc(pgandroid_in_buf, pgandroid_in_cap);
    if (!pgandroid_in_buf)
        abort();
}

static void
ensure_out_capacity(int needed)
{
    if (pgandroid_out_cap >= needed)
        return;
    pgandroid_out_cap = needed * 2;
    pgandroid_out_buf = realloc(pgandroid_out_buf, pgandroid_out_cap);
    if (!pgandroid_out_buf)
        abort();
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * pgandroid_io_init — allocate initial buffers.
 * Called once from pgandroid_init() before the PostgreSQL backend starts.
 */
void
pgandroid_io_init(void)
{
    pthread_mutex_lock(&io_mutex);
    if (!pgandroid_in_buf)
    {
        pgandroid_in_cap = PGANDROID_IN_BUF_INIT;
        pgandroid_in_buf = malloc(pgandroid_in_cap);
    }
    if (!pgandroid_out_buf)
    {
        pgandroid_out_cap = PGANDROID_OUT_BUF_INIT;
        pgandroid_out_buf = malloc(pgandroid_out_cap);
    }
    pgandroid_in_len  = 0;
    pgandroid_in_pos  = 0;
    pgandroid_out_len = 0;
    io_in_ready  = false;
    io_out_ready = false;
    io_closed    = false;
    pthread_mutex_unlock(&io_mutex);
}

/*
 * pgandroid_io_put_input — inject a pre-built PostgreSQL frontend wire-protocol
 * message into the receive buffer.  Signals the backend thread that input is ready.
 *
 * For a simple query, the caller should pass:
 *   data[0]   = 'Q'                     (message type)
 *   data[1-4] = big-endian int32 length  (includes 4 bytes for itself + sql + NUL)
 *   data[5..] = sql string + '\0'
 */
void
pgandroid_io_put_input(const char *data, int len)
{
    pthread_mutex_lock(&io_mutex);
    ensure_in_capacity(len + 1);
    memcpy(pgandroid_in_buf, data, len);
    pgandroid_in_buf[len] = '\0';
    pgandroid_in_len = len;
    pgandroid_in_pos = 0;
    io_in_ready  = true;
    io_out_ready = false;
    pthread_cond_signal(&io_in_cond);
    pthread_mutex_unlock(&io_mutex);
}

/*
 * pgandroid_io_read — called by the patched pq_recvbuf() in pqcomm.c to fill
 * PostgreSQL's receive buffer from our in-memory input buffer.
 *
 * When no input is available:
 *   - Signals io_out_cond (the previous query's result is now in the output buffer)
 *   - Blocks on io_in_cond waiting for the next query
 *
 * Returns number of bytes copied, or 0 at shutdown (EOF equivalent).
 */
int
pgandroid_io_read(char *dest, int maxlen)
{
    int remaining, ncopy;

    pthread_mutex_lock(&io_mutex);

    /*
     * If no input is ready, signal that the previous query's output is
     * complete (the backend has written its response and is now asking for
     * the next query), then wait for new input.
     */
    if (!io_in_ready && !io_closed)
    {
        io_out_ready = true;
        pthread_cond_signal(&io_out_cond);

        while (!io_in_ready && !io_closed)
            pthread_cond_wait(&io_in_cond, &io_mutex);
    }

    if (io_closed)
    {
        /* Shutdown: signal out_cond so the main thread doesn't wait forever */
        io_out_ready = true;
        pthread_cond_signal(&io_out_cond);
        pthread_mutex_unlock(&io_mutex);
        return 0;   /* EOF */
    }

    remaining = pgandroid_in_len - pgandroid_in_pos;
    if (remaining <= 0)
    {
        io_in_ready = false;
        pthread_mutex_unlock(&io_mutex);
        return 0;   /* should not happen */
    }

    ncopy = (remaining < maxlen) ? remaining : maxlen;
    memcpy(dest, pgandroid_in_buf + pgandroid_in_pos, ncopy);
    pgandroid_in_pos += ncopy;

    if (pgandroid_in_pos >= pgandroid_in_len)
        io_in_ready = false;   /* consumed all input */

    pthread_mutex_unlock(&io_mutex);
    return ncopy;
}

/*
 * pgandroid_io_write — called by the patched internal_flush() in pqcomm.c to
 * accumulate PostgreSQL's response bytes into the output buffer.
 */
void
pgandroid_io_write(const char *data, int len)
{
    pthread_mutex_lock(&io_mutex);
    int newlen = pgandroid_out_len + len;
    ensure_out_capacity(newlen + 1);
    memcpy(pgandroid_out_buf + pgandroid_out_len, data, len);
    pgandroid_out_len = newlen;
    pgandroid_out_buf[pgandroid_out_len] = '\0';
    pthread_mutex_unlock(&io_mutex);
}

/*
 * pgandroid_io_get_output — retrieve the accumulated response.
 * Returns a pointer to the internal output buffer (valid until next reset).
 * Sets *out_len to the number of bytes.
 *
 * Safe to call from the main thread after pgandroid_io_wait_result() returns,
 * because the backend is then blocked waiting for the next query.
 */
char *
pgandroid_io_get_output(int *out_len)
{
    if (out_len)
        *out_len = pgandroid_out_len;
    return pgandroid_out_buf ? pgandroid_out_buf : (char *) "";
}

/*
 * pgandroid_io_wait_result — block the calling (JNI/main) thread until the
 * backend has finished processing the current query.
 *
 * The backend signals io_out_cond when it tries to read the next query and
 * finds no input waiting (i.e., after it has written its full response).
 */
void
pgandroid_io_wait_result(void)
{
    pthread_mutex_lock(&io_mutex);
    while (!io_out_ready && !io_closed)
        pthread_cond_wait(&io_out_cond, &io_mutex);
    io_out_ready = false;
    pthread_mutex_unlock(&io_mutex);
}

/*
 * pgandroid_io_reset — clear both input and output buffers.
 * Call before injecting the next query.
 */
void
pgandroid_io_reset(void)
{
    pthread_mutex_lock(&io_mutex);
    pgandroid_in_len  = 0;
    pgandroid_in_pos  = 0;
    pgandroid_out_len = 0;
    io_in_ready  = false;
    io_out_ready = false;
    /* Keep allocated capacity to avoid thrashing for the next query */
    pthread_mutex_unlock(&io_mutex);
}

/*
 * pgandroid_io_close — signal the backend thread to terminate.
 * Called from pgandroid_close().
 */
void
pgandroid_io_close(void)
{
    pthread_mutex_lock(&io_mutex);
    io_closed = true;
    pthread_cond_broadcast(&io_in_cond);
    pthread_cond_broadcast(&io_out_cond);
    pthread_mutex_unlock(&io_mutex);
}

/* -----------------------------------------------------------------------
 * Android libc compatibility stubs
 *
 * backtrace() and friends are in <execinfo.h> (glibc) / Android API ≥33.
 * Devices running Android < 13 (API < 33) do not have these symbols in
 * Bionic libc.  PostgreSQL's elog.c calls them when HAVE_BACKTRACE_SYMBOLS
 * is set (which it is, because configure found execinfo.h in the NDK
 * sysroot headers even though the runtime symbols are absent on older
 * devices).
 *
 * We provide stub implementations here so the linker resolves them
 * internally rather than looking for them in libc at runtime.  The stubs
 * simply return empty results — stack traces are not needed for embedded use.
 * ----------------------------------------------------------------------- */
int
backtrace(void **buffer, int size)
{
    (void) buffer;
    (void) size;
    return 0;
}

char **
backtrace_symbols(void *const *buffer, int size)
{
    (void) buffer;
    (void) size;
    return 0;
}

void
backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    (void) buffer;
    (void) size;
    (void) fd;
}

/* -----------------------------------------------------------------------
 * libpq API stubs
 *
 * string_utils.c (fe_utils) references several libpq functions (PQmblen,
 * PQclientEncoding, PQescapeStringConn, PQserverVersion) to handle
 * connection-aware string quoting.  In embedded pgandroid there is no
 * actual libpq connection object, and initdb only calls the connection-less
 * variants (appendStringLiteral, not appendStringLiteralConn).  We provide
 * stub implementations that return sensible defaults so the linker is
 * satisfied and the code never actually invokes these paths.
 * ----------------------------------------------------------------------- */
typedef struct PGconn PGconn;   /* opaque — we never allocate one */

int
PQclientEncoding(const PGconn *conn)
{
    (void) conn;
    return 6;   /* PG_UTF8 */
}

int
PQserverVersion(const PGconn *conn)
{
    (void) conn;
    return 170000;  /* PostgreSQL 17 */
}

int
PQmblen(const char *s, int encoding)
{
    (void) encoding;
    /* Minimal multi-byte length: treat as single-byte for stub purposes */
    if (!s || (unsigned char)*s < 0x80)
        return 1;
    if ((unsigned char)*s < 0xE0)
        return 2;
    if ((unsigned char)*s < 0xF0)
        return 3;
    return 4;
}

int
PQmblenBounded(const char *s, int encoding)
{
    return PQmblen(s, encoding);
}

size_t
PQescapeStringConn(PGconn *conn, char *to, const char *from, size_t length,
                   int *error)
{
    size_t i, j;
    (void) conn;
    if (error)
        *error = 0;
    for (i = 0, j = 0; i < length && from[i]; i++)
    {
        if (from[i] == '\'' || from[i] == '\\')
            to[j++] = from[i];  /* double-up special chars */
        to[j++] = from[i];
    }
    to[j] = '\0';
    return j;
}

/* -----------------------------------------------------------------------
 * pg_malloc_extended — frontend memory allocator stub.
 *
 * fe_memutils.c provides pg_malloc_extended() as part of the common
 * frontend runtime.  initdb uses it during its setup phase.  We stub it
 * here rather than compiling the full fe_memutils.c to avoid pulling in
 * additional frontend dependencies.
 *
 * Flag bit: MCXT_ALLOC_ZERO (0x04) — zero the allocation.
 * ----------------------------------------------------------------------- */
#define MCXT_ALLOC_ZERO 0x04

void *
pg_malloc_extended(size_t size, int flags)
{
    void *ptr;
    if (size == 0)
        size = 1;
    ptr = malloc(size);
    if (!ptr)
        abort();
    if (flags & MCXT_ALLOC_ZERO)
        memset(ptr, 0, size);
    return ptr;
}

/* -----------------------------------------------------------------------
 * select_default_timezone — timezone detection stub.
 *
 * initdb calls this to pick the default timezone for the cluster.
 * On Android, the system timezone is managed by the Java layer; pgandroid
 * runs the PostgreSQL backend in UTC so timezone conversion is handled by
 * the application.  Returning "UTC" is always safe for initdb.
 * ----------------------------------------------------------------------- */
const char *
select_default_timezone(const char *share_path)
{
    (void) share_path;
    return "UTC";
}

/* -----------------------------------------------------------------------
 * get_restricted_token — Windows privilege-restriction stub.
 *
 * On Windows this function re-launches the process with a restricted token.
 * On all other platforms it is a no-op (defined in restricted_token.c).
 * We provide the stub here because restricted_token.c is not compiled into
 * the backend _srv.o objects used by the link step, and initdb.c calls it.
 * ----------------------------------------------------------------------- */
void
get_restricted_token(void)
{
    /* no-op on Android */
}

/* -----------------------------------------------------------------------
 * pqsignal_fe — frontend signal-handler setter.
 *
 * initdb calls pqsignal_fe() (which is pqsignal() compiled with -DFRONTEND)
 * to install signal handlers for SIGINT/SIGTERM/etc.  In the embedded JNI
 * context we install the handler via sigaction with SA_RESTART just like the
 * real implementation, so that at least graceful cancellation works.
 * ----------------------------------------------------------------------- */
#include <signal.h>

typedef void (*pqsigfunc)(int);

pqsigfunc
pqsignal_fe(int signo, pqsigfunc func)
{
    struct sigaction act, oact;
    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    if (sigaction(signo, &act, &oact) < 0)
        return SIG_ERR;
    return oact.sa_handler;
}
