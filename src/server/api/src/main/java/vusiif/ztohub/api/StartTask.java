package vusiif.ztohub.api;

/**
 * Optional plugin initialization work executed off the host event thread.
 */
@FunctionalInterface
public interface StartTask {
    StartResult start() throws Exception;
}
