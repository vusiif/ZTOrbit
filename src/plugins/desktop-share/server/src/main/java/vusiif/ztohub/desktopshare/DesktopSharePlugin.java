package vusiif.ztohub.desktopshare;

import vusiif.ztohub.desktopshare.room.HostServicesGateway;
import vusiif.ztohub.desktopshare.room.RoomControlConfig;
import vusiif.ztohub.desktopshare.room.RoomController;
import vusiif.ztohub.desktopshare.room.SecureRoomCodeGenerator;
import vusiif.ztohub.api.CancellationToken;
import vusiif.ztohub.api.HostServices;
import vusiif.ztohub.api.LogLevel;
import vusiif.ztohub.api.PluginInfo;
import vusiif.ztohub.api.ServerPlugin;
import vusiif.ztohub.api.StartResult;
import vusiif.ztohub.api.StartTask;

/**
 * Desktop-share server entry point. The plugin only bridges the public host callbacks
 * to the event-thread-confined {@link RoomController}; no private host class is
 * referenced and no work is scheduled outside the host event thread.
 */
public final class DesktopSharePlugin implements ServerPlugin {
    private final PluginInfo info = new PluginInfo("desktop-share", "桌面共享", "0.1.0");
    private volatile RoomController controller;

    @Override
    public PluginInfo info() {
        return info;
    }

    @Override
    public StartTask onStart(HostServices host, CancellationToken cancellation) {
        return () -> {
            cancellation.throwIfCancellationRequested();
            RoomControlConfig.LoadResult loaded = RoomControlConfig.read(host.files());
            if (cancellation.isCancellationRequested()) {
                return StartResult.failure("桌面共享初始化已取消");
            }
            if (!loaded.ok()) {
                String detail = loaded.error();
                host.logs().write(LogLevel.ERROR,
                        "desktop-share 容量配置无效：" + detail
                                + "；请修正 plugins-data/" + info.name()
                                + "/" + RoomControlConfig.CONFIG_FILE + " 后重启服务端");
                return StartResult.failure("容量配置无效：" + detail);
            }
            RoomController created = new RoomController(
                    new HostServicesGateway(host),
                    new SecureRoomCodeGenerator(),
                    System::nanoTime,
                    loaded.config().toTuning());
            created.start();
            if (cancellation.isCancellationRequested()) {
                created.stop();
                return StartResult.failure("桌面共享初始化已取消");
            }
            controller = created;
            host.logs().write(LogLevel.INFO, loaded.present()
                    ? "读取 " + RoomControlConfig.CONFIG_FILE + "：" + describe(loaded.config())
                    : "未找到 " + RoomControlConfig.CONFIG_FILE + "，使用默认容量："
                            + describe(RoomControlConfig.defaults()));
            host.logs().write(LogLevel.INFO,
                    "desktop-share 阶段2 房间控制已启动（route=desktop-share）");
            return StartResult.ok();
        };
    }

    @Override
    public void onTcpPacket(long connectionId, byte[] payload) {
        RoomController current = controller;
        if (current != null) {
            current.onTcpPacket(connectionId, payload);
        }
    }

    @Override
    public void onUdpPacket(java.net.InetSocketAddress endpoint, byte[] payload) {
        RoomController current = controller;
        if (current != null) {
            current.onUdpPacket(endpoint, payload);
        }
    }

    @Override
    public void onConnectionClosed(long connectionId) {
        RoomController current = controller;
        if (current != null) {
            current.onConnectionClosed(connectionId);
        }
    }

    @Override
    public void onStop() {
        RoomController current = controller;
        controller = null;
        if (current != null) {
            current.stop();
        }
    }

    private static String describe(RoomControlConfig config) {
        return "maxRooms=" + config.maxRooms()
                + " maxViewersPerRoom=" + config.maxViewersPerRoom()
                + " maxMembersPerServer=" + config.maxMembersPerServer()
                + " egressBudgetMbps=" + config.egressBudgetMbps()
                + " controlRequestsPerSecond=" + config.controlRequestsPerSecond()
                + " controlRequestBurst=" + config.controlRequestBurst()
                + " joinFailureLimitPerMinute=" + config.joinFailureLimitPerMinute()
                + " joinFailureSources=" + config.joinFailureSources();
    }
}
