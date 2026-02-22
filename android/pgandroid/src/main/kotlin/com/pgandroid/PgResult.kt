package com.pgandroid

import org.json.JSONArray
import org.json.JSONObject
import org.json.JSONTokener

/**
 * The result of a SQL query executed via [PgDatabase.execute].
 *
 * For SELECT queries [rows] contains the result rows and [columns] the column
 * names in order.  For DML/DDL statements [rows] is empty and [rowsAffected]
 * reflects the number of rows changed.
 */
data class PgResult(
    /** Column names in result order. Empty for non-SELECT statements. */
    val columns: List<String>,
    /**
     * Result rows as ordered maps from column name to value.
     *
     * Value types mirror what PostgreSQL returns as JSON:
     *   - [String]  for text/varchar/numeric/date/etc.
     *   - [Int] / [Long] for integer columns (within JSON number range)
     *   - [Double] for floating-point columns
     *   - [Boolean] for boolean columns
     *   - `null` for SQL NULL
     */
    val rows: List<Map<String, Any?>>,
    /** Number of rows inserted/updated/deleted. 0 for SELECT and DDL. */
    val rowsAffected: Int
) {
    companion object {
        /**
         * Parse a JSON result string returned by the JNI bridge.
         *
         * Expected formats (see jni/pgandroid.h for authoritative spec):
         *
         * SELECT success:
         * ```json
         * {"status":"ok","rows":[{"col":"val",...}],"rowcount":N}
         * ```
         *
         * DML/DDL success:
         * ```json
         * {"status":"ok","command":"INSERT 0 1","rowcount":N}
         * ```
         *
         * Error responses are NOT parsed here — the JNI bridge converts them
         * to [PgException] before returning to Kotlin.
         *
         * @throws PgException if the JSON contains `"status":"error"`.
         * @throws IllegalArgumentException if the JSON is malformed.
         */
        fun fromJson(json: String): PgResult {
            val obj = try {
                JSONObject(JSONTokener(json))
            } catch (e: Exception) {
                throw IllegalArgumentException("pgandroid: malformed result JSON: $json", e)
            }

            val status = obj.optString("status", "ok")
            if (status == "error") {
                val message  = obj.optString("message", "unknown PostgreSQL error")
                val sqlstate = obj.optString("sqlstate").takeIf { it.isNotEmpty() }
                throw PgException(message, sqlstate)
            }

            val rowcount = obj.optInt("rowcount", 0)

            val rowsArray: JSONArray? = obj.optJSONArray("rows")
            if (rowsArray == null || rowsArray.length() == 0) {
                return PgResult(emptyList(), emptyList(), rowcount)
            }

            // Collect column names from the first row
            val firstRow = rowsArray.getJSONObject(0)
            val columns = buildList {
                val keys = firstRow.keys()
                while (keys.hasNext()) add(keys.next())
            }

            val rows = buildList(rowsArray.length()) {
                for (i in 0 until rowsArray.length()) {
                    val rowObj = rowsArray.getJSONObject(i)
                    val row = LinkedHashMap<String, Any?>(columns.size)
                    for (col in columns) {
                        row[col] = when {
                            rowObj.isNull(col) -> null
                            else -> rowObj.get(col).let { v ->
                                // org.json returns Integer, Long, Double, Boolean, or String
                                when (v) {
                                    is Int     -> v
                                    is Long    -> v
                                    is Double  -> v
                                    is Boolean -> v
                                    else       -> v.toString()
                                }
                            }
                        }
                    }
                    add(row)
                }
            }

            return PgResult(columns, rows, rowcount)
        }
    }
}
