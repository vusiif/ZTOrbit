#pragma once
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#include <functional>
namespace vusiif::ztgui::voice_chat {
struct AudioDeviceSnapshot {
    bool ready = false;
    QList<QAudioDevice> inputs, outputs;
};
class AudioDeviceCache {
    mutable QMutex mutex;
    AudioDeviceSnapshot devices;
public:
    // QMediaDevices getters are thread-safe. Never call this from page construction.
    bool refresh(const std::function<bool()> &canceled = {}) {
        const auto stopping = [&] { return canceled && canceled(); };
        if (stopping()) return false;
        AudioDeviceSnapshot next;
        next.inputs = QMediaDevices::audioInputs();
        if (stopping()) return false;
        next.outputs = QMediaDevices::audioOutputs();
        if (stopping()) return false;
        next.ready = true;
        QMutexLocker lock(&mutex); devices = std::move(next);
        return true;
    }
    AudioDeviceSnapshot snapshot() const {
        QMutexLocker lock(&mutex); return devices;
    }
};
}
