package vusiif.ztohub.desktopshare.protocol;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * ZDS1 media contract. One business payload is an 88-byte big-endian header plus
 * at most 1125 fragment bytes. With the 19-byte ZTS1 outer frame the complete UDP
 * datagram is at most 1232 bytes.
 */
public final class MediaProtocol {
    public static final int VERSION = 1;
    public static final int HEADER_BYTES = 88;
    public static final int MAX_PACKET_BYTES = 1024 * 1024;
    public static final int MAX_FRAGMENTS = 1024;
    public static final int MAX_FRAGMENT_BYTES = 1125;
    public static final int MAX_UDP_DATAGRAM_BYTES = 1232;
    public static final int ZTS1_OUTER_BYTES = 6 + 13;
    public static final int MAX_PAYLOAD_BYTES = MAX_UDP_DATAGRAM_BYTES - ZTS1_OUTER_BYTES;
    private static final int KNOWN_FLAGS = Flag.KEY_FRAME.mask | Flag.DISCONTINUITY.mask
            | Flag.HAS_DTS.mask;

    private MediaProtocol() {}

    public enum Type {
        REGISTER(1),
        ACK(2),
        VIDEO(3);

        private final int wire;

        Type(int wire) {
            this.wire = wire;
        }

        public int wireValue() {
            return wire;
        }

        static Type fromWire(int value) {
            for (Type type : values()) {
                if (type.wire == value) return type;
            }
            return null;
        }
    }

    public enum Flag {
        KEY_FRAME(0x0001),
        DISCONTINUITY(0x0002),
        HAS_DTS(0x0004);

        private final int mask;

        Flag(int mask) {
            this.mask = mask;
        }

        public int mask() {
            return mask;
        }
    }

    public enum Error {
        NONE("NONE"),
        TOO_SHORT("TOO_SHORT"),
        TOO_LONG("TOO_LONG"),
        BAD_MAGIC("BAD_MAGIC"),
        BAD_VERSION("BAD_VERSION"),
        BAD_TYPE("BAD_TYPE"),
        BAD_FLAGS("BAD_FLAGS"),
        BAD_RESERVED("BAD_RESERVED"),
        BAD_TOKEN("BAD_TOKEN"),
        BAD_SESSION("BAD_SESSION"),
        BAD_ROOM("BAD_ROOM"),
        BAD_EPOCH("BAD_EPOCH"),
        BAD_PACKET_BYTES("BAD_PACKET_BYTES"),
        BAD_FRAGMENT_COUNT("BAD_FRAGMENT_COUNT"),
        BAD_FRAGMENT_INDEX("BAD_FRAGMENT_INDEX"),
        BAD_FRAGMENT_BYTES("BAD_FRAGMENT_BYTES"),
        BAD_LENGTH("BAD_LENGTH"),
        BAD_TIMESTAMP("BAD_TIMESTAMP"),
        REGISTER_FIELDS("REGISTER_FIELDS"),
        BAD_ROUTE("BAD_ROUTE"),
        OVERFLOW("OVERFLOW");

        private final String wire;

        Error(String wire) {
            this.wire = wire;
        }

        public String wireName() {
            return wire;
        }
    }

    public record Header(Type type, int flags, byte[] token, long sessionId, long roomId,
                         long streamEpoch, long sequence, long pts, long dts, long duration,
                         long packetBytes, int fragmentIndex, int fragmentCount,
                         int fragmentBytes) {
        public boolean keyFrame() {
            return (flags & Flag.KEY_FRAME.mask) != 0;
        }

        public boolean discontinuity() {
            return (flags & Flag.DISCONTINUITY.mask) != 0;
        }

        public boolean hasDts() {
            return (flags & Flag.HAS_DTS.mask) != 0;
        }
    }

    public record Packet(int flags, byte[] token, long sessionId, long roomId, long streamEpoch,
                         long sequence, long pts, long dts, long duration, byte[] data) {}

    public record Decoded(Header header, byte[] fragment) {}

    public static final class MediaException extends Exception {
        private final Error error;

        public MediaException(Error error, String message) {
            super(message);
            this.error = error;
        }

        public Error error() {
            return error;
        }
    }

    private static MediaException fail(Error error, String message) {
        return new MediaException(error, message);
    }

    private static boolean tokenSet(byte[] token) {
        if (token == null || token.length != 16) return false;
        for (byte value : token) {
            if (value != 0) return true;
        }
        return false;
    }

    private static long expectedFragmentCount(long packetBytes) {
        return (packetBytes + MAX_FRAGMENT_BYTES - 1) / MAX_FRAGMENT_BYTES;
    }

    private static int expectedFragmentBytes(long packetBytes, int index) {
        long consumed = (long) index * MAX_FRAGMENT_BYTES;
        if (consumed >= packetBytes) return 0;
        return (int) Math.min(MAX_FRAGMENT_BYTES, packetBytes - consumed);
    }

    private static void validateVideoHeader(Header header) throws MediaException {
        if (header.type() != Type.VIDEO) throw fail(Error.BAD_TYPE, "不是视频类型");
        if ((header.flags() & ~KNOWN_FLAGS) != 0) throw fail(Error.BAD_FLAGS, "flags 含未知位");
        if (!tokenSet(header.token())) throw fail(Error.BAD_TOKEN, "媒体 token 不能全为零");
        if (header.sessionId() == 0) throw fail(Error.BAD_SESSION, "会话 ID 不能为零");
        if (header.roomId() == 0) throw fail(Error.BAD_ROOM, "房间 ID 不能为零");
        if (header.streamEpoch() == 0) throw fail(Error.BAD_EPOCH, "流代次不能为零");
        if (header.packetBytes() <= 0 || header.packetBytes() > MAX_PACKET_BYTES)
            throw fail(Error.BAD_PACKET_BYTES, "编码包大小必须在 1..1048576 字节");
        long count = expectedFragmentCount(header.packetBytes());
        if (header.fragmentCount() != count || header.fragmentCount() <= 0
                || header.fragmentCount() > MAX_FRAGMENTS)
            throw fail(Error.BAD_FRAGMENT_COUNT, "片数与编码包大小不一致");
        if (header.fragmentIndex() < 0 || header.fragmentIndex() >= header.fragmentCount())
            throw fail(Error.BAD_FRAGMENT_INDEX, "片索引超出片数");
        int expected = expectedFragmentBytes(header.packetBytes(), header.fragmentIndex());
        if (header.fragmentBytes() != expected)
            throw fail(Error.BAD_FRAGMENT_BYTES, "片长度与编码包大小和片索引不一致");
        if (!header.hasDts() && header.dts() != 0)
            throw fail(Error.BAD_TIMESTAMP, "未设置 DTS 标志时 dts 必须为零");
    }

    private static void validateRegisterHeader(Header header) throws MediaException {
        if (!tokenSet(header.token())) throw fail(Error.BAD_TOKEN, "媒体 token 不能全为零");
        if (header.sessionId() == 0) throw fail(Error.BAD_SESSION, "会话 ID 不能为零");
        if (header.roomId() == 0) throw fail(Error.BAD_ROOM, "房间 ID 不能为零");
        if (header.flags() != 0 || header.streamEpoch() != 0 || header.sequence() != 0
                || header.pts() != 0 || header.dts() != 0 || header.duration() != 0
                || header.packetBytes() != 0 || header.fragmentIndex() != 0
                || header.fragmentCount() != 0 || header.fragmentBytes() != 0)
            throw fail(Error.REGISTER_FIELDS, "注册包除 token、会话和房间外必须为零");
    }

    private static byte[] encodeHeader(Header header) {
        ByteBuffer buffer = ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.BIG_ENDIAN);
        buffer.put((byte) 'Z').put((byte) 'D').put((byte) 'S').put((byte) '1');
        buffer.put((byte) VERSION);
        buffer.put((byte) header.type().wireValue());
        buffer.putShort((short) header.flags());
        buffer.put(header.token(), 0, 16);
        buffer.putLong(header.sessionId());
        buffer.putLong(header.roomId());
        buffer.putLong(header.streamEpoch());
        buffer.putLong(header.sequence());
        buffer.putLong(header.pts());
        buffer.putLong(header.dts());
        buffer.putInt((int) header.duration());
        buffer.putInt((int) header.packetBytes());
        buffer.putShort((short) header.fragmentIndex());
        buffer.putShort((short) header.fragmentCount());
        buffer.putShort((short) header.fragmentBytes());
        buffer.putShort((short) 0);
        return buffer.array();
    }

    public static byte[] encodeRegister(byte[] token, long sessionId, long roomId)
            throws MediaException {
        Header header = new Header(Type.REGISTER, 0, token, sessionId, roomId, 0, 0, 0, 0, 0, 0,
                0, 0, 0);
        validateRegisterHeader(header);
        return encodeHeader(header);
    }

    public static byte[] encodeAck(byte[] token, long sessionId, long roomId)
            throws MediaException {
        Header header = new Header(Type.ACK, 0, token, sessionId, roomId, 0, 0, 0, 0, 0, 0,
                0, 0, 0);
        validateRegisterHeader(header);
        return encodeHeader(header);
    }

    public static Decoded decode(byte[] payload) throws MediaException {
        if (payload == null || payload.length < HEADER_BYTES)
            throw fail(Error.TOO_SHORT, "媒体负载短于 88 字节固定头");
        if (payload.length > MAX_PAYLOAD_BYTES)
            throw fail(Error.TOO_LONG, "媒体负载超过单条 1232 字节 UDP 的上限");
        if (payload[0] != 'Z' || payload[1] != 'D' || payload[2] != 'S' || payload[3] != '1')
            throw fail(Error.BAD_MAGIC, "媒体 magic 不是 ZDS1");
        if ((payload[4] & 0xFF) != VERSION) throw fail(Error.BAD_VERSION, "媒体协议版本不是 1");
        Type type = Type.fromWire(payload[5] & 0xFF);
        if (type == null) throw fail(Error.BAD_TYPE, "未知媒体包类型");
        ByteBuffer buffer = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN);
        buffer.position(6);
        int flags = buffer.getShort() & 0xFFFF;
        byte[] token = new byte[16];
        buffer.get(token);
        long sessionId = buffer.getLong();
        long roomId = buffer.getLong();
        long streamEpoch = buffer.getLong();
        long sequence = buffer.getLong();
        long pts = buffer.getLong();
        long dts = buffer.getLong();
        long duration = buffer.getInt() & 0xFFFF_FFFFL;
        long packetBytes = buffer.getInt() & 0xFFFF_FFFFL;
        int fragmentIndex = buffer.getShort() & 0xFFFF;
        int fragmentCount = buffer.getShort() & 0xFFFF;
        int fragmentBytes = buffer.getShort() & 0xFFFF;
        int reserved = buffer.getShort() & 0xFFFF;
        if (reserved != 0) throw fail(Error.BAD_RESERVED, "保留字段必须为零");
        if ((flags & ~KNOWN_FLAGS) != 0) throw fail(Error.BAD_FLAGS, "flags 含未知位");
        Header header = new Header(type, flags, token, sessionId, roomId, streamEpoch, sequence,
                pts, dts, duration, packetBytes, fragmentIndex, fragmentCount, fragmentBytes);
        if (type == Type.REGISTER || type == Type.ACK) {
            if (payload.length != HEADER_BYTES)
                throw fail(Error.BAD_LENGTH, "注册包不能携带负载");
            validateRegisterHeader(header);
            return new Decoded(header, new byte[0]);
        }
        validateVideoHeader(header);
        int actualBytes = payload.length - HEADER_BYTES;
        if (actualBytes != header.fragmentBytes())
            throw fail(Error.BAD_LENGTH, "实际片段长度与 fragmentBytes 不一致");
        byte[] fragment = Arrays.copyOfRange(payload, HEADER_BYTES, payload.length);
        return new Decoded(header, fragment);
    }

    public static byte[] encodeVideo(Header header, byte[] fragment) throws MediaException {
        if (fragment == null || fragment.length > MAX_FRAGMENT_BYTES)
            throw fail(Error.TOO_LONG, "单片超过 1125 字节");
        Header normalized = new Header(Type.VIDEO, header.flags(), header.token(),
                header.sessionId(), header.roomId(), header.streamEpoch(), header.sequence(),
                header.pts(), header.dts(), header.duration(), header.packetBytes(),
                header.fragmentIndex(), header.fragmentCount(), fragment.length);
        validateVideoHeader(normalized);
        byte[] output = new byte[HEADER_BYTES + fragment.length];
        System.arraycopy(encodeHeader(normalized), 0, output, 0, HEADER_BYTES);
        System.arraycopy(fragment, 0, output, HEADER_BYTES, fragment.length);
        if (output.length > MAX_PAYLOAD_BYTES)
            throw fail(Error.TOO_LONG, "媒体负载超过 1213 字节");
        return output;
    }

    /**
     * Rebuilds one forwarded fragment with a new target token and session ID. The
     * encoded payload and every other header field stay byte-identical; the server never
     * reassembles, decodes, re-encodes or caches video.
     */
    public static byte[] retarget(Header source, byte[] token, long sessionId, byte[] fragment)
            throws MediaException {
        if (source.type() != Type.VIDEO) throw fail(Error.BAD_TYPE, "只能转发视频片段");
        Header target = new Header(Type.VIDEO, source.flags(), token, sessionId,
                source.roomId(), source.streamEpoch(), source.sequence(), source.pts(),
                source.dts(), source.duration(), source.packetBytes(), source.fragmentIndex(),
                source.fragmentCount(), source.fragmentBytes());
        return encodeVideo(target, fragment);
    }

    public static List<byte[]> fragmentVideo(Packet packet) throws MediaException {        if (packet == null || packet.data() == null || packet.data().length == 0)
            throw fail(Error.BAD_PACKET_BYTES, "编码包不能为空");
        if (packet.data().length > MAX_PACKET_BYTES)
            throw fail(Error.BAD_PACKET_BYTES, "编码包超过 1 MiB 上限");
        long packetBytes = packet.data().length;
        long count = expectedFragmentCount(packetBytes);
        if (count <= 0 || count > MAX_FRAGMENTS)
            throw fail(Error.BAD_FRAGMENT_COUNT, "片数超过 1024 上限");
        List<byte[]> fragments = new ArrayList<>((int) count);
        for (int index = 0; index < (int) count; ++index) {
            int length = expectedFragmentBytes(packetBytes, index);
            byte[] fragment = Arrays.copyOfRange(packet.data(), index * MAX_FRAGMENT_BYTES,
                    index * MAX_FRAGMENT_BYTES + length);
            Header header = new Header(Type.VIDEO, packet.flags(), packet.token(),
                    packet.sessionId(), packet.roomId(), packet.streamEpoch(), packet.sequence(),
                    packet.pts(), packet.dts(), packet.duration(), packetBytes, index, (int) count,
                    length);
            fragments.add(encodeVideo(header, fragment));
        }
        return fragments;
    }

    public static boolean fitsDatagram(int payloadBytes) {
        return payloadBytes > 0 && payloadBytes <= MAX_PAYLOAD_BYTES;
    }

    public static int datagramBytes(int payloadBytes) {
        return ZTS1_OUTER_BYTES + payloadBytes;
    }

    public static boolean sameFragmentMetadata(Header left, Header right) {
        return left.type() == right.type() && left.flags() == right.flags()
                && Arrays.equals(left.token(), right.token())
                && left.sessionId() == right.sessionId() && left.roomId() == right.roomId()
                && left.streamEpoch() == right.streamEpoch()
                && left.sequence() == right.sequence() && left.pts() == right.pts()
                && left.dts() == right.dts() && left.duration() == right.duration()
                && left.packetBytes() == right.packetBytes();
    }
}
