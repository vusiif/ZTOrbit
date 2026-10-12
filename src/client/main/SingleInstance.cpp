#include "SingleInstance.h"
#include <QLocalSocket>
#include <QStandardPaths>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace vusiif::ztgui::platform {
SingleInstance::SingleInstance(QObject *parent, const QString &identitySuffix) : QObject(parent) {
    QString identity = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
#ifdef Q_OS_WIN
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    identity += '/' + QString::number(session);
#else
    identity += '/' + qEnvironmentVariable("DISPLAY") + '/' + qEnvironmentVariable("WAYLAND_DISPLAY");
#endif
    identity += identitySuffix;
    m_key = "ztgui-instance-" + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    m_lock = std::make_unique<QLockFile>(QDir::temp().filePath(m_key + ".lock"));
    m_lock->setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (m_server.hasPendingConnections()) {
            auto *socket = m_server.nextPendingConnection();
            socket->setParent(this);
            auto received = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket, received] {
                *received += socket->readAll();
                if (received->size() > 128) { socket->abort(); return; }
                if (!received->contains('\n')) return;
                const auto fields = received->trimmed().split(' ');
                if (fields.size() != 2 || fields[0] != "activate") { socket->abort(); return; }
                socket->write("ok\n"); socket->flush(); socket->disconnectFromServer();
                if (m_activate) m_activate(); else m_pending = true;
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, [socket] { socket->abort(); socket->deleteLater(); });
        }
    });
}
SingleInstance::Result SingleInstance::acquire() {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 10000) {
        if (m_lock->tryLock()) {
            // Only the lock owner can remove an abandoned Unix socket.
            QLocalServer::removeServer(m_key);
            if (!m_server.listen(m_key)) { m_error = m_server.errorString(); m_lock->unlock(); return Result::Failed; }
            return Result::Primary;
        }
        qint64 pid = 0; QString host, app;
        if (m_lock->getLockInfo(&pid, &host, &app)) {
#ifdef Q_OS_WIN
            AllowSetForegroundWindow(DWORD(pid));
#endif
        }
        QLocalSocket socket;
        socket.connectToServer(m_key);
        if (socket.waitForConnected(250)) {
#ifdef Q_OS_WIN
            // Grant the primary immediately before notifying it; never grant the
            // secondary from the primary, which can revoke this permission.
            if (m_lock->getLockInfo(&pid, &host, &app)) AllowSetForegroundWindow(DWORD(pid));
#endif
            socket.write("activate " + QByteArray::number(QCoreApplication::applicationPid()) + '\n');
            socket.flush();
            if (socket.waitForReadyRead(1000) && socket.readAll().startsWith("ok")) return Result::Activated;
        }
        QThread::msleep(50);
    }
    m_error = QStringLiteral("已有程序正在启动或退出，请稍后重试。");
    return Result::Failed;
}
void SingleInstance::setActivation(std::function<void()> activate) {
    m_activate = std::move(activate);
    if (m_pending && m_activate) { m_pending = false; QTimer::singleShot(0, this, [this] { m_activate(); }); }
}
void SingleInstance::activateWindow(QWidget &window) {
    if (window.isMinimized()) window.showNormal(); else window.show();
    window.raise(); window.activateWindow();
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HWND>(window.winId());
    SetForegroundWindow(handle); SetFocus(handle);
#endif
}
}
