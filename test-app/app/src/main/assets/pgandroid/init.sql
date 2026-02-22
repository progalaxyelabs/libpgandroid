-- pgandroid/init.sql
-- Applied by PgMigrator as schema version 1.
--
-- Creates the base schema used by the smoke-test suite.
-- All statements are idempotent (IF NOT EXISTS / OR REPLACE) so this file
-- is safe to apply on a database that was partially initialised.

CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

CREATE TABLE IF NOT EXISTS products (
    id    SERIAL          PRIMARY KEY,
    name  TEXT            NOT NULL,
    price NUMERIC(10, 2)
);

CREATE OR REPLACE FUNCTION get_products()
RETURNS SETOF products
LANGUAGE plpgsql AS $$
BEGIN
    RETURN QUERY SELECT * FROM products ORDER BY name;
END;
$$;
