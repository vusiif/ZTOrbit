package vusiif.ztohub.main.server;

import io.netty.bootstrap.Bootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.socket.DatagramPacket;
import io.netty.channel.socket.nio.NioDatagramChannel;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import vusiif.ztohub.main.ServerHost;

/** Main UDP listener on the service port; one ZTS1 packet per datagram. */
public final class UdpServer {
    private UdpServer() {}

    public static Channel bind(EventLoopGroup group, InetAddress address, int port, ServerHost host)
            throws InterruptedException {
        Bootstrap bootstrap = new Bootstrap();
        bootstrap.group(group)
                .channel(NioDatagramChannel.class)
                .option(ChannelOption.SO_REUSEADDR, true)
                .handler(new ChannelInboundHandlerAdapter() {
                    @Override
                    public void channelRead(ChannelHandlerContext context, Object message) {
                        DatagramPacket packet = (DatagramPacket) message;
                        try {
                            ByteBuf content = packet.content();
                            int size = content.readableBytes();
                            if (size > Wire.MAX_UDP_BYTES) {
                                return;
                            }
                            byte[] bytes = new byte[size];
                            content.readBytes(bytes);
                            InetSocketAddress sender = packet.sender();
                            host.handleDatagram(sender, bytes);
                        } finally {
                            packet.content().release();
                        }
                    }

                    @Override
                    public void exceptionCaught(ChannelHandlerContext context, Throwable failure) {
                        host.log().warn("UDP listener error: " + failure.getMessage());
                    }
                });
        return bootstrap.bind(address, port).sync().channel();
    }
}
