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
#include <QList>
#include <map>
#include <memory>
#ifdef Q_OS_WIN
#include "WindowsWindowCapture.h"
#endif
namespace vusiif::zto::media::engine {
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
    bool play(const QString &track, const QString &device, QVector<float> samples);
    // Read-only view of one clip track in a device mix: frames still waiting in
    // the per-track queue and bytes converted but not yet written to the sink.
    // output is false when the device disappeared; sinkActive is false when the
    // sink stopped unexpectedly.
    struct PlaybackState {
        int queuedFrames = 0;
        qint64 pendingBytes = 0;
        bool output = false;
        bool sinkActive = false;
    };
    PlaybackState playbackState(const QString &device, const QString &track) const;
    bool outputAvailable(const QString &device) const;
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
#ifdef Q_OS_WIN
        std::unique_ptr<WindowsWindowCapture> nativeWindow;
        WindowsWindowCapture::Window nativeWindowInfo;
#endif
        std::unique_ptr<QMediaCaptureSession> session;
        std::unique_ptr<QVideoSink> sink;
        std::unique_ptr<QObject> callbacks = std::make_unique<QObject>();
        // Set once when a device-level failure was reported to the consumers;
        // no further frames are delivered for this source.
        bool failed = false;
        quint64 rawFrames = 0, cpuFrames = 0, deliveredFrames = 0;
        quint64 cachedDeliveries = 0;
        QVideoFrame lastVideoFrame;
        // Consecutive one-second window-capture probes in which the captured
        // window was not listed as capturable. A single miss (minimized, being
        // moved, occluded) only increments this counter.
        int missingSeconds = 0;
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
        int drainTicks = 0;
    };
    QMediaDevices devices;
    QTimer timer;
    // Validity probe for active window captures; runs only while such a source
    // is attached. One missed enumeration is not evidence that the window was
    // closed, so the failure threshold is a sustained absence.
    QTimer windowTimer;
    static constexpr int WindowMissingSeconds = 10;
#ifdef Q_OS_WIN
    using CapturableWindow = WindowsWindowCapture::Window;
#else
    using CapturableWindow = QCapturableWindow;
#endif
    QList<CapturableWindow> lastWindows;
    std::map<QString, std::unique_ptr<Source>> inputs;
    std::map<QString, std::unique_ptr<Output>> outputs;
    QMap<QString, CapturableWindow> windows;
    quint64 rawVideoFrames = 0, cpuVideoFrames = 0;
    void mix();
    void monitorWindows();
    void screenGone(const QString &name);
    void deliverVideo(Source &source, const QVideoFrame &frame);
    static void failSource(Source &source, const QString &message);
    static QAudioDevice audioDevice(bool input, const QString &id);
    // Test-only injection used by media integration tests through a file whose
    // path is supplied in ZTO_MEDIA_TEST_CLIP_FAILURE. Production processes
    // never set that variable, so this stays inert.
    bool injectedPlaybackFailure();
    QString m_testFailureNonce;
    int m_testAcceptedPlays = 0;
};
} // namespace vusiif::zto::media::engine
