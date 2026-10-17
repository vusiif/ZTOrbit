package vusiif.ztgui.server.main;

import java.io.Closeable;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.OffsetDateTime;
import java.time.format.DateTimeFormatter;
import java.time.temporal.ChronoUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Process-lifetime UTF-8 log file called {@code logs/YYMMDD-N.log}. The sequence starts after the
 * largest existing number for the current date and the filename never changes across midnight.
 */
public final class DailyLogFile implements Closeable {
    private static final DateTimeFormatter DATE = DateTimeFormatter.ofPattern("yyMMdd");
    private static final DateTimeFormatter ISO_MILLIS = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSSXXX");
    private final Path path;
    private final OutputStream output;

    private DailyLogFile(Path path, OutputStream output) {
        this.path = path;
        this.output = output;
    }

    public static DailyLogFile open(Path directory) throws IOException {
        Files.createDirectories(directory);
        String date = LocalDate.now().format(DATE);
        long sequence = 0;
        Pattern pattern = Pattern.compile("^" + date + "-([1-9][0-9]*)\\.log$");
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(directory)) {
            for (Path entry : entries) {
                Matcher match = pattern.matcher(entry.getFileName().toString());
                if (!match.matches()) {
                    continue;
                }
                try {
                    sequence = Math.max(sequence, Long.parseLong(match.group(1)));
                } catch (NumberFormatException ignored) {
                    // Ignore unparsable names; the maximum valid sequence still wins.
                }
            }
        }
        while (sequence < Long.MAX_VALUE) {
            sequence++;
            Path candidate = directory.resolve(date + "-" + sequence + ".log");
            try {
                return new DailyLogFile(
                        candidate, Files.newOutputStream(candidate, java.nio.file.StandardOpenOption.CREATE_NEW));
            } catch (FileAlreadyExistsException race) {
                // Another process reserved this name; try the next sequence number.
            } catch (IOException failure) {
                throw new IOException("Cannot open application log: " + failure.getMessage(), failure);
            }
        }
        throw new IOException("Application log sequence exhausted");
    }

    public Path path() {
        return path;
    }

    /** Writes one timestamped line and returns an error description, or empty on success. */
    public synchronized String writeChecked(String text) {
        String line = OffsetDateTime.now().truncatedTo(ChronoUnit.MILLIS).format(ISO_MILLIS)
                + " " + text.strip() + "\n";
        try {
            output.write(line.getBytes(StandardCharsets.UTF_8));
            output.flush();
            return "";
        } catch (IOException failure) {
            return failure.getMessage() == null ? failure.toString() : failure.getMessage();
        }
    }

    @Override
    public synchronized void close() {
        try {
            output.close();
        } catch (IOException ignored) {
            // Closing the log is best effort during shutdown.
        }
    }

    static String dateForNow() {
        return LocalDateTime.now().format(DATE);
    }
}
