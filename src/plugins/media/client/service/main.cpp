#include "SessionEngine.h"
#include <QGuiApplication>
#include <QDebug>
#include <QThread>
using namespace vusiif::ztgui;
int main(int argc, char **argv) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QGuiApplication app(argc, argv);
    protocol::service::Worker worker(
        QJsonArray{QJsonObject{{"service", media::SessionService}, {"api", media::SessionApi}}});
    media::engine::ExtensionRegistry extensions;
    auto root = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../..");
    auto error = extensions.load(root);
    if (!error.isEmpty()) {
        qCritical().noquote() << error;
        return 3;
    }
    QThread deviceThread;
    QObject deviceDispatch;
    deviceDispatch.moveToThread(&deviceThread);
    deviceThread.start();
    media::engine::DeviceManager *devices = nullptr;
    std::unique_ptr<media::engine::SessionEngine> engine;
    worker.onCall = [&](const QString &lease, const QString &, const QString &method, const QJsonObject &data,
                        const QByteArray &, protocol::service::Worker::Reply reply) {
        if (engine)
            engine->call(lease, method, data, std::move(reply));
        else
            reply({}, {}, "Media engine not ready");
    };
    worker.onData = [&](const QString &lease, const QJsonObject &header, const QByteArray &data) {
        if (engine)
            engine->submit(lease, header, data);
    };
    worker.onReleaseAsync = [&](const QString &lease, protocol::service::Worker::Reply reply) {
        if (engine)
            engine->release(lease, std::move(reply));
        else
            reply({}, {}, {});
    };
    worker.onShutdown = [&] {
        if (engine)
            engine->shutdown();
    };
    worker.deferReady();
    worker.onConnected = [&] {
        QMetaObject::invokeMethod(&deviceDispatch, [&] {
            devices = new media::engine::DeviceManager();
            QMediaDevices::audioInputs();
            QMediaDevices::audioOutputs();
            QMediaDevices::videoInputs();
            QMetaObject::invokeMethod(&worker, [&] {
                engine = std::make_unique<media::engine::SessionEngine>(worker, *devices, extensions);
                worker.markReady();
            });
        });
    };
    int result = 2;
    if (worker.start(app.arguments()))
        result = app.exec();
    engine.reset();
    QMetaObject::invokeMethod(
        &deviceDispatch,
        [&] {
            delete devices;
            devices = nullptr;
            deviceDispatch.moveToThread(app.thread());
        },
        Qt::BlockingQueuedConnection);
    deviceThread.quit();
    deviceThread.wait();
    return result;
}
