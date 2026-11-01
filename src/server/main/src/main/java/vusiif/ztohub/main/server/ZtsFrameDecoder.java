package vusiif.ztohub.main.server;

import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.handler.codec.ByteToMessageDecoder;
import io.netty.handler.codec.CorruptedFrameException;
import java.util.List;

/** Decodes 4-byte big-endian length-prefixed ZTS1 frames, including split and coalesced reads. */
public final class ZtsFrameDecoder extends ByteToMessageDecoder {
    @Override
    protected void decode(ChannelHandlerContext context, ByteBuf input, List<Object> output) {
        if (input.readableBytes() < 4) {
            return;
        }
        input.markReaderIndex();
        int length = input.readInt();
        if (length < 7 || length > Wire.MAX_TCP_BYTES) {
            throw new CorruptedFrameException("invalid frame length");
        }
        if (input.readableBytes() < length) {
            input.resetReaderIndex();
            return;
        }
        byte[] bytes = new byte[length];
        input.readBytes(bytes);
        Wire.Packet packet = Wire.decode(bytes);
        if (packet == null) {
            throw new CorruptedFrameException("invalid frame");
        }
        output.add(packet);
    }
}
