package vusiif.ztohub.main.server;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.netty.buffer.Unpooled;
import io.netty.channel.embedded.EmbeddedChannel;
import org.junit.jupiter.api.Test;

class ZtsFrameDecoderTest {
    @Test
    void waitsForSplitFrames() {
        EmbeddedChannel channel = new EmbeddedChannel(new ZtsFrameDecoder());
        byte[] framed = Wire.frame("route", new byte[] {1, 2, 3});
        assertFalse(channel.writeInbound(Unpooled.wrappedBuffer(framed, 0, 3)));
        assertFalse(channel.writeInbound(Unpooled.wrappedBuffer(framed, 3, 5)));
        assertTrue(channel.writeInbound(
                Unpooled.wrappedBuffer(framed, 8, framed.length - 8)));
        Wire.Packet packet = channel.readInbound();
        assertInstanceOf(Wire.Packet.class, packet);
        assertArrayEquals(new byte[] {1, 2, 3}, packet.payload());
        channel.finishAndReleaseAll();
    }

    @Test
    void decodesCoalescedFrames() {
        EmbeddedChannel channel = new EmbeddedChannel(new ZtsFrameDecoder());
        byte[] first = Wire.frame("a", new byte[] {1});
        byte[] second = Wire.frame("b", new byte[] {2});
        byte[] both = new byte[first.length + second.length];
        System.arraycopy(first, 0, both, 0, first.length);
        System.arraycopy(second, 0, both, first.length, second.length);
        assertTrue(channel.writeInbound(Unpooled.wrappedBuffer(both)));
        Wire.Packet one = channel.readInbound();
        Wire.Packet two = channel.readInbound();
        assertInstanceOf(Wire.Packet.class, one);
        assertInstanceOf(Wire.Packet.class, two);
        channel.finishAndReleaseAll();
    }

    @Test
    void rejectsInvalidLengthsAndPackets() {
        EmbeddedChannel channel = new EmbeddedChannel(new ZtsFrameDecoder());
        assertThrows(Exception.class, () -> channel.writeInbound(Unpooled.copyInt(6)));
        channel.finishAndReleaseAll();

        EmbeddedChannel oversized = new EmbeddedChannel(new ZtsFrameDecoder());
        assertThrows(Exception.class,
                () -> oversized.writeInbound(Unpooled.copyInt(Wire.MAX_TCP_BYTES + 1)));
        oversized.finishAndReleaseAll();

        EmbeddedChannel garbage = new EmbeddedChannel(new ZtsFrameDecoder());
        byte[] invalid = "garbage".getBytes();
        assertThrows(Exception.class, () -> garbage.writeInbound(
                Unpooled.wrappedBuffer(Unpooled.copyInt(invalid.length).array(), invalid)));
        garbage.finishAndReleaseAll();    }
}
