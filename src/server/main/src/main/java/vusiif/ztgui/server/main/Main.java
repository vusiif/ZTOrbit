package vusiif.ztgui.server.main;

import java.io.IOException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * ZTServer command line entry point.
 *
 * <p>Usage errors exit 2, bind/startup failures exit 1 and a clean SIGTERM/SIGINT shutdown exits 0.
 * The server does not install or uninstall ZeroTier/ztncui and does not build Moons; those actions
 * moved to {@code zt-installer.sh}.
 */
public final class Main {
    private static final String LEGACY_MESSAGE =
            "Install/uninstall is handled by ./zt-installer.sh; see DEPLOYMENT.md";

    private Main() {}

    public static void main(String[] args) {
        int status = run(args);
        if (status != 0) {
            System.exit(status);
        }
    }

    static int run(String[] args) {
        Options options;
        try {
            options = Options.parse(args);
        } catch (UsageException usage) {
            System.err.println(usage.getMessage());
            return 2;
        }
        if (options.help || options.arguments.isEmpty()) {
            printHelp();
            return 0;
        }
        if (options.version) {
            System.out.println(ServerVersion.current().version());
            return 0;
        }
        if (options.installerHint) {
            System.err.println(LEGACY_MESSAGE);
            return 2;
        }
        if (!options.server) {
            System.err.println("Choose the --server operation");
            return 2;
        }

        Path directory = Path.of("").toAbsolutePath().normalize();
        HostLog log = new HostLog(directory);
        log.writeStartup(ServerVersion.current().version());
        ServerHost host = new ServerHost(directory, options.listen, options.port, log);

        // Register the hook before binding so a SIGTERM during startup also shuts down cleanly.
        CountDownLatch stopped = new CountDownLatch(1);
        AtomicBoolean cleaned = new AtomicBoolean();
        Thread shutdown = new Thread(() -> {
            if (cleaned.compareAndSet(false, true)) {
                host.stop();
                host.close();
                log.close();
            }
            stopped.countDown();
            if (ownsShutdown()) {
                // A signal-initiated shutdown is a normal stop for systemd.
                Runtime.getRuntime().halt(0);
            }
        }, "zt-shutdown");
        boolean hookRegistered = false;
        try {
            Runtime.getRuntime().addShutdownHook(shutdown);
            hookRegistered = true;
        } catch (IllegalStateException alreadyShuttingDown) {
            log.warn("Cannot register shutdown hook; JVM is already shutting down");
        }

        try {
            host.start();
        } catch (IOException failure) {
            String message = failure.getMessage() == null ? failure.toString() : failure.getMessage();
            log.error(message);
            System.err.println(message);
            if (hookRegistered) {
                try {
                    Runtime.getRuntime().removeShutdownHook(shutdown);
                } catch (IllegalStateException alreadyShuttingDown) {
                    // The JVM is already stopping; the hook will run and clean up.
                }
            }
            if (cleaned.compareAndSet(false, true)) {
                host.close();
                log.close();
            }
            return 1;
        }

        try {
            stopped.await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
        if (cleaned.compareAndSet(false, true)) {
            host.stop();
            host.close();
            log.close();
        }
        return 0;
    }

    /** True when this process is running as the test fixture; tests never halt the JVM. */
    private static boolean ownsShutdown() {
        return !Boolean.getBoolean("ztserver.test.noHalt");
    }

    private static void printHelp() {
        System.out.println("""
                ZTServer %s - ZeroTier Linux plugin server (Java 25 + Netty)

                Usage:
                  ./start-server.sh [--server] [--listen <IPv4>] [--port <1-65535>]
                  ./start-server.sh --version
                  ./start-server.sh --help
                  ./start-server.sh --check-deps

                Options:
                  --server            Run the server in the foreground (default listen 0.0.0.0)
                  --listen <IPv4>     Listen address; IPv4 literals only (default 0.0.0.0)
                  --port <port>       TCP/UDP port, 1-65535 (default 18100)
                  --version           Print the server version and exit
                  --help              Print this help

                --listen and --port require --server. This program no longer installs, uninstalls
                or exports Moons; use the standalone zt-installer.sh and see DEPLOYMENT.md.
                """.formatted(ServerVersion.current().version()));
    }

    private static final class UsageException extends Exception {
        UsageException(String message) {
            super(message);
        }
    }

    private static final class Options {
        boolean server;
        boolean help;
        boolean version;
        boolean installerHint;
        int port = 18100;
        InetAddress listen;
        final java.util.List<String> arguments = new java.util.ArrayList<>();

        static Options parse(String[] args) throws UsageException {
            Options options = new Options();
            boolean portSet = false;
            boolean listenSet = false;
            String listenText = null;
            for (int index = 0; index < args.length; index++) {
                String argument = args[index];
                options.arguments.add(argument);
                switch (argument) {
                    case "--help", "-h" -> options.help = true;
                    case "--version", "-v" -> options.version = true;
                    case "--server" -> options.server = true;
                    case "--listen" -> {
                        String value = requireValue(args, ++index, "--listen");
                        listenText = value;
                        listenSet = true;
                    }
                    case "--port" -> {
                        String value = requireValue(args, ++index, "--port");
                        options.port = parsePort(value);
                        portSet = true;
                    }
                    case "--check-deps" -> {
                        throw new UsageException(
                                "--check-deps is handled by ./start-server.sh, not by zt-server.jar");
                    }
                    case "-i", "--install", "-u", "--uninstall" -> {
                        options.installerHint = true;
                        // Consume the optional zerotier/ztncui/all target, matching the old CLI.
                        if (index + 1 < args.length && !args[index + 1].startsWith("-")) {
                            index++;
                        }
                    }
                    case "-m", "--moon" -> options.installerHint = true;
                    case "--ip" -> {
                        options.installerHint = true;
                        if (index + 1 < args.length) {
                            index++;
                        }
                    }
                    case "--force", "--yes", "--dry-run" -> options.installerHint = true;
                    default -> {
                        if (argument.startsWith("--listen=")) {
                            listenText = argument.substring("--listen=".length());
                            listenSet = true;
                        } else if (argument.startsWith("--port=")) {
                            options.port = parsePort(argument.substring("--port=".length()));
                            portSet = true;
                        } else if (argument.startsWith("--install=")
                                || argument.startsWith("--uninstall=")
                                || argument.startsWith("--moon=")
                                || argument.startsWith("--ip=")) {
                            options.installerHint = true;
                        } else if (argument.startsWith("-")) {
                            throw new UsageException("Unknown option: " + argument);
                        } else {
                            throw new UsageException("Unexpected argument: " + argument);
                        }
                    }
                }
            }
            if (options.help || options.version) {
                return options;
            }
            if (options.installerHint) {
                return options;
            }
            if (options.arguments.isEmpty()) {
                return options;
            }
            if (!options.server && (portSet || listenSet)) {
                throw new UsageException("Listen/port require --server");
            }
            if (!options.server) {
                throw new UsageException("Choose the --server operation");
            }
            options.listen = listenText == null
                    ? parseListen("0.0.0.0")
                    : parseListen(listenText);
            return options;
        }

        private static String requireValue(String[] args, int index, String option)
                throws UsageException {
            if (index >= args.length) {
                throw new UsageException(option + " requires a value");
            }
            return args[index];
        }

        private static int parsePort(String value) throws UsageException {
            int port;
            try {
                port = Integer.parseInt(value);
            } catch (NumberFormatException invalid) {
                throw new UsageException("Invalid port: " + value);
            }
            if (port < 1 || port > 65535) {
                throw new UsageException("Invalid port: " + value);
            }
            return port;
        }

        private static InetAddress parseListen(String value) throws UsageException {
            if (!value.matches("\\d{1,3}(?:\\.\\d{1,3}){3}")) {
                throw new UsageException("Invalid IPv4 listen address: " + value);
            }
            try {
                InetAddress address = InetAddress.getByName(value);
                if (!(address instanceof Inet4Address)) {
                    throw new UsageException("Invalid IPv4 listen address: " + value);
                }
                return address;
            } catch (IOException invalid) {
                throw new UsageException("Invalid IPv4 listen address: " + value);
            }
        }

        private static boolean isLegacy(String argument) {
            return argument.equals("-i") || argument.equals("-u") || argument.equals("-m")
                    || argument.startsWith("--install")
                    || argument.startsWith("--uninstall")
                    || argument.startsWith("--moon")
                    || argument.equals("--ip")
                    || argument.startsWith("--ip=")
                    || argument.equals("--force")
                    || argument.equals("--yes")
                    || argument.equals("--dry-run");
        }
    }
}