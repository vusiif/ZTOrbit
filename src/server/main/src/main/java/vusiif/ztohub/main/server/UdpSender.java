package vusiif.ztohub.main.server;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFuture;
import io.netty.channel.EventLoop;
import io.netty.channel.socket.DatagramPacket;
import io.netty.util.ReferenceCountUtil;
import java.net.InetSocketAddress;

/**
 * Submits one ZTS1 datagram to the UDP channel under the global {@link UdpSendBudget}.
 *
 * <p>A cross-thread send reserves the budget <em>before</em> queuing a task on the Netty
 * event loop. The write completion listener releases the reservation exactly once for
 * success, failure and cancellation. A submission error releases it immediately. The
 * class does not decide policy beyond budget admission: {@code true} only means the
 * datagram was accepted for transmission, never that it was sent or delivered.
 */
public final class UdpSender {
    /** Test seam: production submits through {@link #runOnEventLoop}. */
    @FunctionalInterface
    interface Submission {
        void submit(EventLoop loop, Runnable task);
    }

    private final UdpSendBudget budget;
    private final Submission submission;

    public UdpSender(UdpSendBudget budget) {
        this(budget, UdpSender::runOnEventLoop);
    }

    UdpSender(UdpSendBudget budget, Submission submission) {
        this.budget = budget;
        this.submission = submission;
    }

    /**
     * Reserves and submits one already-encoded ZTS1 datagram.
     *
     * @return {@code false} when the channel is inactive, the host budget rejected the
     *         datagram, or the event loop rejected the task; {@code true} means accepted
     *         for transmission
     */
    public boolean send(Channel channel, InetSocketAddress endpoint, byte[] packet) {
        if (channel == null || endpoint == null || packet == null || !channel.isActive()) {
            return false;
        }
        UdpSendBudget.Reservation reservation = budget.reserve(packet.length);
        if (reservation == null) {
            return false;
        }
        try {
            submission.submit(channel.eventLoop(),
                    () -> write(channel, endpoint, packet, reservation));
            return true;
        } catch (RuntimeException failure) {
            // The task never ran; the reservation must not leak.
            reservation.release();
            return false;
        }
    }

    private static void write(Channel channel, InetSocketAddress endpoint, byte[] packet,
            UdpSendBudget.Reservation reservation) {
        try {
            if (!channel.isActive()) {
                reservation.release();
                return;
            }
            ByteBuf buffer = Unpooled.wrappedBuffer(packet);
            ChannelFuture future;
            try {
                future = channel.writeAndFlush(new DatagramPacket(buffer, endpoint));
            } catch (RuntimeException failure) {
                ReferenceCountUtil.safeRelease(buffer);
                reservation.release();
                return;
            }
            // Netty completes the promise for success, failure and cancellation.
            future.addListener(ignored -> reservation.release());
        } catch (Throwable failure) {
            // No failure path may leave the reservation (or its buffer) behind.
            reservation.release();
        }
    }

    private static void runOnEventLoop(EventLoop loop, Runnable task) {
        if (loop.inEventLoop()) {
            task.run();
        } else {
            loop.execute(task);
        }
    }
}
