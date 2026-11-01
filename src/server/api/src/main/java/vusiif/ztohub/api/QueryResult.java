package vusiif.ztohub.api;

import com.fasterxml.jackson.databind.JsonNode;

/**
 * Result of a ZeroTier read-only query.
 *
 * @param document parsed JSON document, or {@code null} when the query failed
 * @param error error description, or {@code null}/empty when the query succeeded
 */
public record QueryResult(JsonNode document, String error) {
    public QueryResult {
        error = error == null ? "" : error;
    }

    public boolean ok() {
        return error.isEmpty() && document != null;
    }

    public static QueryResult success(JsonNode document) {
        return new QueryResult(document, "");
    }

    public static QueryResult failure(String error) {
        return new QueryResult(null, error);
    }
}
