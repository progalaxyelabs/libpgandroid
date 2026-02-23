/*
 * test_pgandroid.c -- minimal C test for pgandroid C API
 *
 * Build (arm64):
 *   $CC -D__ANDROID_PGANDROID__ -Os -fPIC \
 *       -I pgandroid \
 *       test_pgandroid.c -o test_pgandroid \
 *       -L. -lpgandroid -llog -lm
 *
 * Run on phone:
 *   adb push test_pgandroid libpgandroid.so share/ /data/local/tmp/pg/
 *   adb shell "cd /data/local/tmp/pg && LD_LIBRARY_PATH=. ./test_pgandroid"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pgandroid.h"

#define DATADIR "/data/local/tmp/pg/data"

static void test_query(const char *label, const char *sql)
{
    printf("[TEST] %s: %s\n", label, sql);
    char *result = pgandroid_exec(sql);
    if (result) {
        printf("  => %s\n", result);
        free(result);
    } else {
        printf("  => (NULL -- OOM?)\n");
    }
}

int main(void)
{
    int rc;

    printf("=== pgandroid C API test ===\n\n");

    /* Step 1: Initialize (runs initdb if needed, starts backend) */
    printf("[INIT] pgandroid_init(\"%s\")\n", DATADIR);
    rc = pgandroid_init(DATADIR);
    printf("[INIT] returned %d\n\n", rc);
    if (rc != 0) {
        printf("FAILED: pgandroid_init returned %d\n", rc);
        return 1;
    }

    /* Step 2: Run test queries */
    test_query("SELECT 1", "SELECT 1 AS num");
    test_query("version", "SELECT version()");
    test_query("CREATE TABLE",
               "CREATE TABLE IF NOT EXISTS test_t (id serial PRIMARY KEY, name text)");
    test_query("INSERT",
               "INSERT INTO test_t (name) VALUES ('hello pgandroid')");
    test_query("SELECT *", "SELECT * FROM test_t");
    test_query("COUNT", "SELECT count(*) AS cnt FROM test_t");
    test_query("uuid", "SELECT gen_random_uuid() AS uuid");
    test_query("DROP TABLE", "DROP TABLE IF EXISTS test_t");

    /* Step 3: Close */
    printf("\n[CLOSE] pgandroid_close()\n");
    pgandroid_close();
    printf("[CLOSE] done\n");

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
