package vusiif.ztgui.server.main.server;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import java.net.InetAddress;
import vusiif.ztgui.server.main.ServerHost;

/** TCP listener bootstrap for the ZTS1 length-framed transport. */
public final class TcpServer {
    private TcpServer() {}

    public static Channel bind(EventLoopGroup group, InetAddress address, int port, ServerHost host)
            throws InterruptedException {
        ServerBootstrap bootstrap = new ServerBootstrap();
        bootstrap.group(group)
                .channel(NioServerSocketChannel.class)
                .option(ChannelOption.SO_REUSEADDR, true)
                .childOption(ChannelOption.TCP_NODELAY, true)
                .childOption(ChannelOption.SO_KEEPALIVE, true)
                .childHandler(new ChannelInitializer<SocketChannel>() {
                    @Override
                    protected void initChannel(SocketChannel channel) {
                        channel.pipeline().addLast(new ZtsFrameDecoder());
                        channel.pipeline().addLast(new TcpConnectionHandler(host));
                    }
                });
        return bootstrap.bind(address, port).sync().channel();
    }
}
