#include "DeviceManager.h"
#include "MediaClock.h"
#include <QCameraDevice>
#include <QCapturableWindow>
#include <QUuid>
#include <algorithm>
#include <cmath>
namespace vusiif::ztgui::media::engine {
DeviceManager::DeviceManager(QObject *parent) : QObject(parent) {
    connect(&devices, &QMediaDevices::audioInputsChanged, this, [this] {
        if (changed)
            changed();
    });
    connect(&devices, &QMediaDevices::audioOutputsChanged, this, [this] {
        if (changed)
            changed();
    });
    connect(&devices, &QMediaDevices::videoInputsChanged, this, [this] {
        if (changed)
            changed();
    });
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(20);
    connect(&timer, &QTimer::timeout, this, [this] { mix(); });
}
QAudioDevice DeviceManager::audioDevice(bool input, const QString &id) {
    auto devices = input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
    auto bytes = QByteArray::fromHex(id.toLatin1());
    for (const auto &d : devices)
        if (d.id() == bytes)
            return d;
    return id.isEmpty() ? (input ? QMediaDevices::defaultAudioInput() : QMediaDevices::defaultAudioOutput())
                        : QAudioDevice{};
}
QJsonArray DeviceManager::sources() {
    QJsonArray result;
    auto add = [&](QString id, QString kind, QString description, bool defaultDevice = false) {
        result.append(QJsonObject{
            {"id", id}, {"kind", kind}, {"description", description}, {"default", defaultDevice}});
    };
    for (const auto &d : QMediaDevices::audioInputs())
        add(QString::fromLatin1(d.id().toHex()), "microphone", d.description(), d.isDefault());
    for (const auto &d : QMediaDevices::audioOutputs())
        add(QString::fromLatin1(d.id().toHex()), "speaker", d.description(), d.isDefault());
    for (const auto &d : QMediaDevices::videoInputs())
        add(QString::fromLatin1(d.id().toHex()), "camera", d.description(), d.isDefault());
    for (auto *s : QGuiApplication::screens())
        add(s->name(), "screen", s->name());
    QMap<QString, QCapturableWindow> current;
    for (const auto &w : QWindowCapture::capturableWindows()) {
        QString id;
        for (auto it = windows.cbegin(); it != windows.cend(); ++it)
            if (it.value() == w) {
                id = it.key();
                break;
            }
        if (id.isEmpty())
            id = QUuid::createUuid().toString(QUuid::Id128);
        current.insert(id, w);
        add(id, "window", w.description());
    }
    windows = std::move(current);
    add("synthetic", "synthetic", "Synthetic video test pattern");
    return result;
}
QString DeviceManager::attach(const QString &track, const TrackConfig &config, Consumer consumer) {
    if (config.direction == "receive")
        return config.kind == "audio" && audioDevice(false, config.output).isNull()
                   ? QString("Audio output unavailable")
                   : QString{};
    if (config.source == "synthetic")
        return {};
    QString kind = config.kind == "audio" ? "microphone" : config.source.section(':', 0, 0);
    QString id = config.kind == "audio" ? config.source : config.source.section(':', 1);
    if (kind == "microphone" && !config.recordAllowed)
        return "Microphone capture permission was not granted";
    if (kind == "microphone") {
        auto d = audioDevice(true, id);
        if (d.isNull())
            return "Audio input unavailable";
        id = QString::fromLatin1(d.id().toHex());
    }
    auto key = kind + ":" + id;
    auto found = inputs.find(key);
    if (found != inputs.end()) {
        const auto &active = found->second->config;
        if (config.kind == "video" &&
            (active.video.width != config.video.width || active.video.height != config.video.height ||
             active.codec.frameRate != config.codec.frameRate))
            return "Source is in use with an incompatible format";
        found->second->consumers.insert(track, std::move(consumer));
        return {};
    }
    auto source = std::make_unique<Source>();
    auto *s = source.get();
    s->id = id;
    s->kind = kind;
    s->config = config;
    s->consumers.insert(track, std::move(consumer));
    auto failed = [s](const QString &message) {
        auto consumers = s->consumers;
        for (const auto &c : consumers)
            if (c.failed)
                c.failed(message);
    };
    if (kind == "microphone") {
        const auto device = audioDevice(true, id);
        auto f = PcmConverter::mono();
        if (!device.isFormatSupported(f))
            f = device.preferredFormat();
        s->format = f;
        s->audio = std::make_unique<QAudioSource>(device, f);
        s->audio->setBufferSize(f.bytesForDuration(100000));
        connect(s->audio.get(), &QAudioSource::stateChanged, s->callbacks.get(),
                [s, failed](QtAudio::State state) {
                    if (state == QtAudio::StoppedState && s->audio->error() != QtAudio::NoError)
                        failed("Microphone capture failed");
                });
        s->capture = s->audio->start();
        if (!s->capture)
            return "Cannot open microphone";
        connect(s->capture, &QIODevice::readyRead, s->callbacks.get(), [s] {
            const auto capturedAt=monotonicTimeUs();
            auto bytes = s->capture->readAll();
            for (const auto &c : s->consumers)
                if (c.audio)
                    c.audio(bytes, s->format,capturedAt);
        });
    } else {
        s->session = std::make_unique<QMediaCaptureSession>();
        s->sink = std::make_unique<QVideoSink>();
        s->session->setVideoSink(s->sink.get());
        connect(s->sink.get(), &QVideoSink::videoFrameChanged, s->callbacks.get(), [s](const QVideoFrame &f) {
            const auto capturedAt=monotonicTimeUs();
            auto cpu = QVideoFrame(f.toImage());
            cpu.setStartTime(capturedAt);
            for (const auto &c : s->consumers)
                if (c.video)
                    c.video(cpu);
        });
        if (kind == "camera") {
            QCameraDevice chosen;
            for (const auto &d : QMediaDevices::videoInputs())
                if (QString::fromLatin1(d.id().toHex()) == id || (id.isEmpty() && d.isDefault())) {
                    chosen = d;
                    break;
                }
            if (chosen.isNull())
                return "Camera unavailable";
            QCameraFormat format;
            for (const auto &f : chosen.videoFormats())
                if (f.resolution() == QSize(config.video.width, config.video.height) &&
                    f.maxFrameRate() >= config.codec.frameRate &&
                    f.minFrameRate() <= config.codec.frameRate) {
                    format = f;
                    break;
                }
            if (format.isNull() && !config.codec.allowAdjustment)
                return "Camera does not support requested capture format";
            s->camera = std::make_unique<QCamera>(chosen);
            if (!format.isNull())
                s->camera->setCameraFormat(format);
            s->session->setCamera(s->camera.get());
            connect(s->camera.get(), &QCamera::errorOccurred, s->callbacks.get(),
                    [failed](QCamera::Error, const QString &m) { failed(m); });
            s->camera->start();
        } else if (kind == "screen") {
            QScreen *screen = nullptr;
            for (auto *d : QGuiApplication::screens())
                if (d->name() == id)
                    screen = d;
            if (!screen)
                return "Screen unavailable";
            s->screen = std::make_unique<QScreenCapture>();
            s->screen->setScreen(screen);
            s->session->setScreenCapture(s->screen.get());
            connect(s->screen.get(), &QScreenCapture::errorOccurred, s->callbacks.get(),
                    [failed](QScreenCapture::Error, const QString &m) { failed(m); });
            s->screen->start();
        } else if (kind == "window") {
            QCapturableWindow window = windows.value(id);
            if (!window.isValid())
                return "Window unavailable; enumerate sources again";
            s->window = std::make_unique<QWindowCapture>();
            s->window->setWindow(window);
            s->session->setWindowCapture(s->window.get());
            connect(s->window.get(), &QWindowCapture::errorOccurred, s->callbacks.get(),
                    [failed](QWindowCapture::Error, const QString &m) { failed(m); });
            s->window->start();
        } else
            return "Unsupported source kind";
    }
    inputs.emplace(key, std::move(source));
    return {};
}
void DeviceManager::detach(const QString &track) {
    for (auto it = inputs.begin(); it != inputs.end();) {
        it->second->consumers.remove(track);
        if (it->second->consumers.isEmpty())
            it = inputs.erase(it);
        else
            ++it;
    }
    for (auto it = outputs.begin(); it != outputs.end();) {
        it->second->tracks.remove(track);
        if (it->second->tracks.isEmpty())
            it = outputs.erase(it);
        else
            ++it;
    }
    if (outputs.empty())
        timer.stop();
}
void DeviceManager::play(const QString &track, const QString &id, QVector<float> samples) {
    auto device = audioDevice(false, id);
    if (device.isNull())
        return;
    auto key = QString::fromLatin1(device.id().toHex());
    auto &output = outputs[key];
    if (!output) {
        output = std::make_unique<Output>();
        auto f = PcmConverter::mono();
        if (!device.isFormatSupported(f))
            f = device.preferredFormat();
        output->format = f;
        output->sink = std::make_unique<QAudioSink>(device, f);
        output->sink->setBufferSize(f.bytesForDuration(100000));
        output->playback = output->sink->start();
    }
    auto &queue = output->tracks[track];
    if (queue.size() >= 5)
        queue.dequeue();
    queue.enqueue(std::move(samples));
    if (!timer.isActive())
        timer.start();
}
void DeviceManager::mix() {
    for (auto &entry : outputs) {
        auto &o = *entry.second;
        QVector<float> mixed(FrameSamples, 0);
        for (auto &q : o.tracks)
            if (!q.isEmpty()) {
                auto frame = q.dequeue();
                for (qsizetype i = 0; i < std::min(mixed.size(), frame.size()); ++i)
                    mixed[i] += frame[i];
            }
        for (auto &s : mixed)
            s = std::isfinite(s) ? std::clamp(s, -1.f, 1.f) : 0.f;
        auto bytes =
            QByteArray(reinterpret_cast<const char *>(mixed.constData()), mixed.size() * sizeof(float));
        o.pending += o.converter.process(bytes, PcmConverter::mono(), o.format);
        auto limit = o.format.bytesForDuration(100000);
        if (o.pending.size() > limit)
            o.pending.remove(0, o.pending.size() - limit);
        if (o.playback) {
            auto n = o.playback->write(o.pending.constData(),
                                       std::min(qint64(o.pending.size()), o.sink->bytesFree()));
            if (n > 0) {
                o.pending.remove(0, n);
                o.bytes += n;
            }
        }
    }
}
QJsonObject DeviceManager::statistics() const {
    quint64 bytes = 0;
    for (const auto &o : outputs)
        bytes += o.second->bytes;
    return {{"physicalInputs", int(inputs.size())},
            {"physicalOutputs", int(outputs.size())},
            {"playedBytes", QString::number(bytes)}};
}
} // namespace vusiif::ztgui::media::engine
