#pragma once
#include <ztmedia/MediaTypes.h>
#include "PcmConverter.h"
#include <QAudioDevice>
#include <QAudioSource>
#include <QAudioSink>
#include <QMediaDevices>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QVideoSink>
#include <QVideoFrame>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QQueue>
#include <QMap>
#include <map>
#include <memory>
namespace vusiif::ztgui::media::engine {
class DeviceManager final : public QObject {
  public:
    struct Consumer {
        std::function<void(QByteArray, QAudioFormat,qint64)> audio;
        std::function<void(QVideoFrame)> video;
        std::function<void(QString)> failed;
    };
    explicit DeviceManager(QObject *parent = nullptr);
    QJsonArray sources();
    QString attach(const QString &track, const TrackConfig &config, Consumer consumer);
    void detach(const QString &track);
    void play(const QString &track, const QString &device, QVector<float> samples);
    std::function<void()> changed;
    QJsonObject statistics() const;

  private:
    struct Source {
        QString id, kind;
        TrackConfig config;
        QMap<QString, Consumer> consumers;
        std::unique_ptr<QAudioSource> audio;
        QIODevice *capture = nullptr;
        QAudioFormat format;
        std::unique_ptr<QCamera> camera;
        std::unique_ptr<QScreenCapture> screen;
        std::unique_ptr<QWindowCapture> window;
        std::unique_ptr<QMediaCaptureSession> session;
        std::unique_ptr<QVideoSink> sink;
        std::unique_ptr<QObject> callbacks = std::make_unique<QObject>();
        ~Source() { callbacks.reset(); }
    };
    struct Output {
        std::unique_ptr<QAudioSink> sink;
        QIODevice *playback = nullptr;
        QAudioFormat format;
        QMap<QString, QQueue<QVector<float>>> tracks;
        QByteArray pending;
        PcmConverter converter;
        quint64 bytes = 0;
    };
    QMediaDevices devices;
    QTimer timer;
    std::map<QString, std::unique_ptr<Source>> inputs;
    std::map<QString, std::unique_ptr<Output>> outputs;
    QMap<QString, QCapturableWindow> windows;
    void mix();
    static QAudioDevice audioDevice(bool input, const QString &id);
};
} // namespace vusiif::ztgui::media::engine
