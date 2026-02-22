-- pgandroid/migrations/002.sql
-- Applied by PgMigrator as schema version 2.
--
-- Adds an audit timestamp to the products table.

ALTER TABLE products
    ADD COLUMN IF NOT EXISTS created_at TIMESTAMPTZ DEFAULT now();
