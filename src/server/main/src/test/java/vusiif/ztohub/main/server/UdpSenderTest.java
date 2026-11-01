package vusiif.ztohub.main.server;

import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelOutboundHandlerAdapter;
import io.netty.channel.ChannelPromise;
import io.netty.channel.embedded.EmbeddedChannel;
import io.netty.util.ReferenceCountUtil;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Wires the real {@link UdpSender} to a channel whose writes never complete. That makes
 * the in-flight counter observable: it only falls when a completion listener runs.
 */
class UdpSenderTest {
    private static final InetSocketAddress ENDPOINT =
            new InetSocketAddress("127.0.0.1", 18100);

    private final List<EmbeddedChannel> channels = new ArrayList<>();

    @AfterEach
    void releaseChannels() {
        PendingWrites.reset();
        for (EmbeddedChannel channel : channels) {
            channel.finishAndReleaseAll();
        }
        channels.clear();
    }

    private EmbeddedChannel hangingChannel() {
        EmbeddedChannel channel = new EmbeddedChannel(new ChannelOutboundHandlerAdapter() {
            @Override
            public void write(ChannelHandlerContext context, Object message, ChannelPromise promise) {
                // Deliberately never forward the write: the promise stays pending.
                PendingWrites.hold(promise, message);
            }
        });
        channels.add(channel);
        return channel;
    }

    @Test
    void reserveHappensBeforeSubmissionAndCapHoldsAcrossThreads() throws Exception {
        UdpSendBudget budget = new UdpSendBudget();
        ConcurrentLinkedQueue<Runnable> tasks = new ConcurrentLinkedQueue<>();
        AtomicBoolean reservedBeforeSubmit = new AtomicBoolean(true);
        UdpSender sender = new UdpSender(budget, (loop, task) -> {
            if (budget.snapshot().inflightBytes() <= 0) {
                reservedBeforeSubmit.set(false);
            }
            tasks.add(task);
        });
        EmbeddedChannel channel = hangingChannel();
        int cost = 1000 + UdpSendBudget.PER_PACKET_OVERHEAD_BYTES;
        int expected = (UdpSendBudget.MAX_INFLIGHT_BYTES - 0) / cost;
        int threads = 8;
        int attempts = 250;
        AtomicInteger accepted = new AtomicInteger();
        CountDownLatch start = new CountDownLatch(1);
        List<Thread> workers = new ArrayList<>();
        for (int index = 0; index < threads; index++) {
            Thread worker = new Thread(() -> {
                try {
                    start.await();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
                for (int attempt = 0; attempt < attempts; attempt++) {
                    if (sender.send(channel, ENDPOINT, new byte[1000])) {
                        accepted.incrementAndGet();
                    }
                }
            }, "udp-sender-" + index);
            workers.add(worker);
            worker.start();
        }
        start.countDown();
        for (Thread worker : workers) {
            worker.join();
        }
        assertEquals(expected, accepted.get());
        assertTrue(reservedBeforeSubmit.get(), "the budget must be reserved before queuing");
        assertEquals(expected, budget.snapshot().inflightPackets());
        assertTrue(budget.snapshot().inflightBytes() <= UdpSendBudget.MAX_INFLIGHT_BYTES);

        // Run the queued event-loop tasks: every write is now suspended on its promise.
        Runnable task;
        while ((task = tasks.poll()) != null) {
            task.run();
        }
        assertEquals(expected, PendingWrites.pending(), "all accepted writes are suspended");
        assertEquals(expected, budget.snapshot().inflightPackets());

        // Completing writes releases exactly one reservation each.
        PendingWrites.succeedAll();
        UdpSendBudget.Snapshot drained = budget.snapshot();
        assertEquals(0, drained.inflightBytes());
        assertEquals(0, drained.inflightPackets());
        assertEquals(0, drained.outstanding());
        assertEquals(expected, drained.accepted());
    }

    @Test
    void successFailureCancellationAndDuplicateCompletionReleaseOnce() {
        UdpSendBudget budget = new UdpSendBudget();
        UdpSender sender = new UdpSender(budget);
        EmbeddedChannel success = hangingChannel();
        EmbeddedChannel failure = hangingChannel();
        EmbeddedChannel cancelled = hangingChannel();
        assertTrue(sender.send(success, ENDPOINT, new byte[20]));
        assertTrue(sender.send(failure, ENDPOINT, new byte[20]));
        assertTrue(sender.send(cancelled, ENDPOINT, new byte[20]));
        success.runPendingTasks();
        failure.runPendingTasks();
        cancelled.runPendingTasks();
        assertEquals(3, PendingWrites.pending());
        assertEquals(3, budget.snapshot().inflightPackets());

        PendingWrites.complete(0, null);
        PendingWrites.complete(1, new IOException("boom"));
        PendingWrites.cancel(2);
        assertEquals(0, budget.snapshot().inflightPackets());
        assertEquals(0, budget.snapshot().inflightBytes());

        // A second completion notification must not drive the counters negative. Netty
        // promises reject a duplicate completion, so a buggy writer that tries twice is
        // represented by the try* results below.
        assertFalse(PendingWrites.complete(0, null));
        assertFalse(PendingWrites.complete(0, new IOException("again")));
        assertFalse(PendingWrites.cancel(2));
        assertEquals(0, budget.snapshot().inflightPackets());
        assertEquals(0, budget.snapshot().inflightBytes());
    }

    @Test
    void submissionExceptionAndClosedChannelReleaseWithoutSending() {
        UdpSendBudget budget = new UdpSendBudget();
        UdpSender rejected = new UdpSender(budget, (loop, task) -> {
            throw new RejectedExecutionException("event loop stopped");
        });
        EmbeddedChannel channel = hangingChannel();
        assertFalse(rejected.send(channel, ENDPOINT, new byte[64]));
        UdpSendBudget.Snapshot snapshot = budget.snapshot();
        assertEquals(0, snapshot.inflightBytes());
        assertEquals(0, snapshot.inflightPackets());
        assertEquals(0, snapshot.outstanding());

        channel.close();
        UdpSender normal = new UdpSender(budget);
        assertFalse(normal.send(channel, ENDPOINT, new byte[64]));
        assertEquals(0, budget.snapshot().inflightBytes());
    }

    @Test
    void immediateWriteFailureReleasesThroughThePromiseListener() {
        UdpSendBudget budget = new UdpSendBudget();
        EmbeddedChannel channel = new EmbeddedChannel(new ChannelOutboundHandlerAdapter() {
            @Override
            public void write(ChannelHandlerContext context, Object message, ChannelPromise promise) {
                ReferenceCountUtil.safeRelease(message);
                promise.setFailure(new IOException("socket failure"));
            }
        });
        channels.add(channel);
        UdpSender sender = new UdpSender(budget);
        assertTrue(sender.send(channel, ENDPOINT, new byte[128]));
        channel.runPendingTasks();
        assertEquals(0, budget.snapshot().inflightBytes());
        assertEquals(0, budget.snapshot().inflightPackets());
    }

    /** Test-local store of suspended writes; keeps the message buffer for release. */
    private static final class PendingWrites {
        private static final List<ChannelPromise> PROMISES = new ArrayList<>();
        private static final List<Object> MESSAGES = new ArrayList<>();

        static synchronized void hold(ChannelPromise promise, Object message) {
            PROMISES.add(promise);
            MESSAGES.add(message);
        }

        static synchronized int pending() {
            return PROMISES.size();
        }

        static synchronized void succeedAll() {
            for (ChannelPromise promise : PROMISES) {
                promise.setSuccess();
            }
            releaseMessages();
            PROMISES.clear();
        }

        static synchronized void reset() {
            releaseMessages();
            PROMISES.clear();
        }

        static synchronized boolean complete(int index, Throwable failure) {
            if (index >= PROMISES.size()) {
                return false;
            }
            ChannelPromise promise = PROMISES.get(index);
            return failure == null ? promise.trySuccess() : promise.tryFailure(failure);
        }

        static synchronized boolean cancel(int index) {
            if (index >= PROMISES.size()) {
                return false;
            }
            return PROMISES.get(index).cancel(false);
        }
        private static void releaseMessages() {
            for (Object message : MESSAGES) {
                if (message instanceof ByteBuf buffer) {
                    ReferenceCountUtil.safeRelease(buffer);
                }
            }
            MESSAGES.clear();
        }
    }
}
