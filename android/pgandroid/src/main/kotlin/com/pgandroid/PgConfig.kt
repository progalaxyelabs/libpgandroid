package com.pgandroid

/**
 * Configuration for an embedded PostgreSQL instance.
 *
 * All memory sizes follow PostgreSQL GUC notation (e.g. "16MB", "2MB").
 * [extensions] lists extension names to load via CREATE EXTENSION at startup.
 *
 * Use [PgConfig.default] for mobile-tuned defaults (low memory footprint).
 */
data class PgConfig(
    /** PostgreSQL shared_buffers (applied at initdb time; cannot change at runtime). */
    val sharedBuffers: String = "16MB",
    /** work_mem — per-sort/hash memory limit. Applied via SET at startup. */
    val workMem: String = "2MB",
    /** wal_buffers — WAL write buffer size. Applied at initdb time. */
    val walBuffers: String = "1MB",
    /** max_wal_size — maximum WAL on disk before forced checkpoint. */
    val maxWalSize: String = "64MB",
    /** Extensions to load on every open (via CREATE EXTENSION IF NOT EXISTS). */
    val extensions: List<String> = listOf("pgcrypto", "uuid-ossp")
) {

    /**
     * Serialize this config to the JSON format expected by [PgDatabase.nativeOpen].
     *
     * Only keys that the JNI bridge recognises are emitted:
     *   shared_buffers, work_mem, wal_buffers, max_wal_size
     */
    fun toJson(): String = buildString {
        append('{')
        append("\"shared_buffers\":\"").append(sharedBuffers.escape()).append('"')
        append(",\"work_mem\":\"").append(workMem.escape()).append('"')
        append(",\"wal_buffers\":\"").append(walBuffers.escape()).append('"')
        append(",\"max_wal_size\":\"").append(maxWalSize.escape()).append('"')
        append('}')
    }

    companion object {
        /**
         * Mobile-tuned defaults: low shared_buffers, small work_mem,
         * limited WAL footprint.
         *
         * See HLD §8.3 for the rationale behind each value.
         */
        fun default(): PgConfig = PgConfig(
            sharedBuffers = "16MB",
            workMem       = "2MB",
            walBuffers    = "1MB",
            maxWalSize    = "64MB",
            extensions    = listOf("pgcrypto", "uuid-ossp")
        )
    }
}

/** Minimal JSON string escape (backslash + double-quote only). */
private fun String.escape(): String = replace("\\", "\\\\").replace("\"", "\\\"")
