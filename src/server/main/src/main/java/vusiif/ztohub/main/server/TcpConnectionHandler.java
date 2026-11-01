package vusiif.ztohub.main.server;

import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.SimpleChannelInboundHandler;
import vusiif.ztohub.main.ServerHost;

/** Routes decoded ZTS1 packets and owns connection registration/cleanup. */
public final class TcpConnectionHandler extends SimpleChannelInboundHandler<Wire.Packet> {
    private final ServerHost host;
    private Connection connection;

    public TcpConnectionHandler(ServerHost host) {
        this.host = host;
    }

    @Override
    public void channelActive(ChannelHandlerContext context) {
        connection = host.registry().add(context.channel(), host.now());
        if (connection == null) {
            host.log().warn("Connection limit reached; rejecting new TCP connection");
            context.close();
            return;
        }
        host.log().debug("TCP connection " + connection.id() + " accepted from "
                + context.channel().remoteAddress());
    }

    @Override
    protected void channelRead0(ChannelHandlerContext context, Wire.Packet packet) {
        Connection current = connection;
        if (current == null) {
            context.close();
            return;
        }
        host.handlePacket(current, packet);
    }

    @Override
    public void channelInactive(ChannelHandlerContext context) {
        Connection current = connection;
        connection = null;
        if (current != null && host.registry().remove(current.id()) != null) {
            host.handleClosed(current);
        }
        host.log().debug("TCP connection closed");
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext context, Throwable failure) {
        host.log().debug("TCP connection error: " + failure.getMessage());
        context.close();
    }
}
