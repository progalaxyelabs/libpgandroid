package com.pgandroid

/**
 * Exception thrown when PostgreSQL returns an error.
 *
 * The message contains the PostgreSQL error text. The [sqlstate] property
 * holds the 5-character SQLSTATE error code (e.g. "42703" for
 * "undefined_column") when available.
 */
class PgException(
    message: String,
    val sqlstate: String? = null
) : Exception(message)
