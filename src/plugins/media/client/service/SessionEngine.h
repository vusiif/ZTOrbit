#pragma once
#include "DeviceManager.h"
#include "ExtensionRegistry.h"
#include <ztprotocol/ServiceWorker.h>
#include <QThread>
#include <atomic>
#include <algorithm>
#include <map>
namespace vusiif::ztgui::media::engine {
class SessionActor;
class SessionEngine final : public QObject {
  public:
    using Reply = protocol::service::Worker::Reply;
    SessionEngine(protocol::service::Worker &worker, DeviceManager &devices, ExtensionRegistry &extensions,
                  QObject *parent = nullptr);
    ~SessionEngine() override;
    void call(const QString &lease, const QString &method, const QJsonObject &data, Reply reply);
    void submit(const QString &lease, const QJsonObject &header, const QByteArray &payload);
    void release(const QString &lease, Reply reply);
    void shutdown();

  private:
    struct QueueCounter {
        std::atomic<int> value{0}, peak{0};
        std::atomic<qint64> bytes{0}, peakBytes{0};
        bool reserve(qsizetype size) {
            auto previous = bytes.fetch_add(size);
            if (previous + size > protocol::service::BulkChannel::QueueBytes) {
                bytes.fetch_sub(size);
                return false;
            }
            auto observed = peakBytes.load();
            while (observed < previous + size &&
                   !peakBytes.compare_exchange_weak(observed, previous + size)) {
            }
            return true;
        }
        void release(qsizetype size) { bytes.fetch_sub(size); }
        int fetch_add(int n) {
            auto previous = value.fetch_add(n);
            auto observed = peak.load();
            auto count = std::min(previous + n, 32);
            while (observed < count && !peak.compare_exchange_weak(observed, count)) {
            }
            return previous;
        }
        void fetch_sub(int n) { value.fetch_sub(n); }
        int load() const { return value.load(); }
    };
    struct Session {
        std::unique_ptr<QThread> thread;
        SessionActor *actor = nullptr;
        QSet<QString> tracks;
        QSet<QString> active;
        QMap<QString, std::shared_ptr<std::atomic<bool>>> operations;
        std::shared_ptr<QueueCounter> queued = std::make_shared<QueueCounter>();
        bool closing = false;
    };
    protocol::service::Worker &worker;
    DeviceManager &devices;
    ExtensionRegistry &extensions;
    std::shared_ptr<std::atomic<int>> videoCount = std::make_shared<std::atomic<int>>(0);
    std::map<QString, std::unique_ptr<Session>> sessions;
    Session *session(const QString &lease);
    void detach(const QString &track) {
        QMetaObject::invokeMethod(&devices, [this, track] { devices.detach(track); });
    }
};
} // namespace vusiif::ztgui::media::engine
