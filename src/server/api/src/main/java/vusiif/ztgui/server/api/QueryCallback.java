package vusiif.ztgui.server.api;

/** Receives a ZeroTier query result on the host event thread. */
@FunctionalInterface
public interface QueryCallback {
    void completed(QueryResult result);
}
