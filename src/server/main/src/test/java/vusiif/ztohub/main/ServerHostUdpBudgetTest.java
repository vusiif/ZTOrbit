package vusiif.ztohub.main;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Real-host wiring test: the budget is applied around the actual Netty UDP channel, the
 * datagram really leaves the socket, and every accepted reservation returns to zero.
 * This does not replace the deterministic suspended-write fixture; it proves the
 * production path is connected to that budget.
 */
class ServerHostUdpBudgetTest {
    @TempDir
    Path workingDirectory;

    @Test
    void realChannelSendsAndReturnsTheBudgetToZero() throws Exception {
        try (DatagramSocket receiver = new DatagramSocket(new InetSocketAddress("127.0.0.1", 0))) {
            receiver.setSoTimeout(5_000);
            ServerHost host = new ServerHost(workingDirectory, InetAddress.getByName("127.0.0.1"),
                    0, new HostLog(workingDirectory));
            host.start();
            try {
                InetSocketAddress endpoint = new InetSocketAddress("127.0.0.1",
                        receiver.getLocalPort());
                byte[] payload = "stage5-udp-budget".getBytes(StandardCharsets.US_ASCII);
                assertTrue(host.sendUdp("desktop-share", endpoint, payload),
                        "a small datagram must fit the empty budget");

                DatagramPacket received = new DatagramPacket(new byte[1232], 1232);
                try {
                    receiver.receive(received);
                } catch (SocketTimeoutException timeout) {
                    throw new AssertionError("the datagram did not reach the UDP socket", timeout);
                }
                byte[] wire = new byte[received.getLength()];
                System.arraycopy(received.getData(), 0, wire, 0, received.getLength());
                assertTrue(wire.length > 6 + "desktop-share".length());
                assertArrayEquals("ZTS1".getBytes(StandardCharsets.US_ASCII),
                        java.util.Arrays.copyOf(wire, 4));
                assertEquals(1, host.udpBudgetSnapshot().accepted());
                awaitZero(host);

                int sends = 200;
                AtomicInteger accepted = new AtomicInteger();
                CountDownLatch start = new CountDownLatch(1);
                List<Thread> senders = new ArrayList<>();
                for (int thread = 0; thread < 4; thread++) {
                    Thread sender = new Thread(() -> {
                        try {
                            start.await();
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            return;
                        }
                        for (int index = 0; index < sends / 4; index++) {
                            if (host.sendUdp("desktop-share", endpoint, payload)) {
                                accepted.incrementAndGet();
                            }
                        }
                    }, "host-udp-" + thread);
                    senders.add(sender);
                    sender.start();
                }
                start.countDown();
                for (Thread sender : senders) {
                    sender.join();
                }
                assertTrue(accepted.get() > 0);
                awaitZero(host);

                UdpSendBudgetSnapshot snapshot = new UdpSendBudgetSnapshot(host);
                assertTrue(snapshot.accepted() >= sends, "all real-channel sends are accounted");
                assertEquals(0, snapshot.inflightBytes());
                assertEquals(0, snapshot.inflightPackets());
                assertEquals(0, snapshot.outstanding());

                // Two plugin routes share one budget; there is no per-plugin allowance.
                long beforeShare = host.udpBudgetSnapshot().accepted();
                for (int index = 0; index < 20; index++) {
                    assertTrue(host.sendUdp(index % 2 == 0 ? "desktop-share" : "voice-chat",
                            endpoint, payload));
                }
                awaitZero(host);
                assertEquals(beforeShare + 20, host.udpBudgetSnapshot().accepted());
                assertEquals(0, host.udpBudgetSnapshot().inflightPackets());
            } finally {
                host.close();
            }
            assertFalse(host.sendUdp("desktop-share",
                    new InetSocketAddress("127.0.0.1", receiver.getLocalPort()),
                    new byte[] {1, 2, 3}), "a stopped host must not accept new datagrams");
            assertEquals(0, host.udpBudgetSnapshot().inflightBytes());
            assertEquals(0, host.udpBudgetSnapshot().inflightPackets());
        }
    }

    private static void awaitZero(ServerHost host) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (host.udpBudgetSnapshot().inflightBytes() == 0
                    && host.udpBudgetSnapshot().inflightPackets() == 0) {
                return;
            }
            Thread.sleep(5);
        }
        throw new AssertionError("the in-flight budget did not return to zero: "
                + host.udpBudgetSnapshot());
    }

    @Test
    void stopRacingWithConcurrentSendersEndsAtZeroWithoutNegativeCounters() throws Exception {
        try (DatagramSocket receiver = new DatagramSocket(new InetSocketAddress("127.0.0.1", 0))) {
            ServerHost host = new ServerHost(workingDirectory, InetAddress.getByName("127.0.0.1"),
                    0, new HostLog(workingDirectory));
            host.start();
            InetSocketAddress endpoint = new InetSocketAddress("127.0.0.1",
                    receiver.getLocalPort());
            CountDownLatch start = new CountDownLatch(1);
            AtomicInteger rejected = new AtomicInteger();
            List<Thread> senders = new ArrayList<>();
            for (int thread = 0; thread < 4; thread++) {
                Thread sender = new Thread(() -> {
                    try {
                        start.await();
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                    for (int index = 0; index < 20_000; index++) {
                        if (!host.sendUdp("desktop-share", endpoint, new byte[64])) {
                            rejected.incrementAndGet();
                        }
                    }
                }, "host-stop-race-" + thread);
                senders.add(sender);
                sender.start();
            }
            start.countDown();
            Thread.sleep(10);
            host.close();
            for (Thread sender : senders) {
                sender.join(10_000);
                assertFalse(sender.isAlive(), "a sender is still blocked after stop");
            }
            UdpSendBudgetSnapshot stopped = new UdpSendBudgetSnapshot(host);
            assertTrue(rejected.get() > 0, "stop must reject later sends");
            assertEquals(0, stopped.inflightBytes());
            assertEquals(0, stopped.inflightPackets());
            assertEquals(0, stopped.outstanding());
            assertTrue(stopped.accepted() >= 0);
            assertTrue(stopped.rejected() > 0);
            assertFalse(host.sendUdp("desktop-share", endpoint, new byte[] {1}));
        }
    }

    /** Small adapter so the assertions read the public snapshot only. */
    private record UdpSendBudgetSnapshot(long inflightBytes, long inflightPackets,
            int outstanding, long accepted, long rejected) {
        UdpSendBudgetSnapshot(ServerHost host) {
            this(host.udpBudgetSnapshot().inflightBytes(),
                    host.udpBudgetSnapshot().inflightPackets(),
                    host.udpBudgetSnapshot().outstanding(),
                    host.udpBudgetSnapshot().accepted(),
                    host.udpBudgetSnapshot().rejected());
        }
    }
}
