#pragma once

#include <ztmedia/MediaTypes.h>
#include <zto/PluginServices.h>
#include <QObject>
#include <QSet>
#include <algorithm>
#include <functional>

namespace vusiif::zto::media {
// Generic short-audio service (media.clip / 1). It has no rooms, members or
// voice concepts: the caller supplies a validated WAV payload and output
// device. One active clip is allowed per lease; queueing belongs to the caller.
class AudioClipClient final : public QObject {
  public:
    std::function<void(const QString &clipId)> finished;
    std::function<void(const QString &clipId, const MediaError &error)> failed;
    explicit AudioClipClient(std::shared_ptr<util::ServiceLease> lease, QObject *parent = nullptr)
        : QObject(parent), m_lease(std::move(lease)) {
        if (!m_lease) return;
        m_lease->subscribe(this, [this](const util::ServiceResult &result) {
            if (!result.ok()) {
                report({}, {"service", result.error});
                return;
            }
            const auto event = result.data["event"].toString();
            const auto clip = result.data["clip"].toString();
            if (event == "finished") {
                m_clips.remove(clip);
                if (finished && !m_closing) {
                    auto callback = finished;
                    callback(clip);
                }
            } else if (event == "failed") {
                m_clips.remove(clip);
                report(clip, {"service", result.data["message"].toString()});
            }
        });
    }
    ~AudioClipClient() override {
        if (m_lease) m_lease->close(nullptr, {});
    }
    quint64 generation() const { return m_lease ? m_lease->generation() : 0; }
    bool available() const { return m_lease && m_lease->available() && !m_failed; }
    // A successful reply means the request was accepted, not that playback has
    // completed; the binary payload is the complete WAV file. Each accepted
    // clip produces exactly one terminal event: finished() on success, failed()
    // when the output disappears, a chunk is rejected or the device mix queue
    // does not drain in time.
    void play(const QString &device, double gain, const QByteArray &wav, QObject *context,
              Completion<QString> done) {
        if (done && !m_lease) {
            done({{}, {"service", "Clip service unavailable"}});
            return;
        }
        call("play", {{"device", device}, {"gain", std::clamp(gain, 0.0, 2.0)}}, wav, context,
             [this, done](const util::ServiceResult &result) {
                 if (!result.ok()) {
                     if (done) done({{}, {"service", result.error}});
                     return;
                 }
                 const auto id = result.data["clipId"].toString();
                 if (!id.isEmpty()) m_clips.insert(id);
                 if (done) done({{id}, {}});
             });
    }
    // Idempotent: an unknown or already finished clip counts as stopped.
    void stop(const QString &clipId, QObject *context, Completion<bool> done = {}) {
        m_clips.remove(clipId);
        call("stop", {{"clipId", clipId}}, {}, context,
             [done](const util::ServiceResult &result) {
                 if (done) done({{result.ok()}, failure(result)});
             });
    }
    void close(QObject *context, Completion<bool> done) {
        m_closing = true;
        finished = {};
        failed = {};
        m_clips.clear();
        if (!m_lease) {
            if (done) done({{true}, {}});
            return;
        }
        m_lease->close(context, [done](const util::ServiceResult &result) {
            if (done) done({{result.ok()}, failure(result)});
        });
    }

  private:
    std::shared_ptr<util::ServiceLease> m_lease;
    QSet<QString> m_clips;
    bool m_closing = false;
    bool m_failed = false;
    static MediaError failure(const util::ServiceResult &result) {
        return result.ok() ? MediaError{} : MediaError{"service", result.error};
    }
    void report(const QString &clip, const MediaError &error) {
        if (m_closing || !failed) return;
        auto callback = failed;
        callback(clip, error);
    }
    void call(const QString &method, const QJsonObject &data, const QByteArray &payload,
              QObject *context, util::ServiceCallback done) {
        if (!m_lease || m_closing || m_failed) {
            if (done) done({{}, {}, "Clip service unavailable"});
            return;
        }
        m_lease->call(method, data, payload, context, std::move(done));
    }
};
} // namespace vusiif::zto::media
