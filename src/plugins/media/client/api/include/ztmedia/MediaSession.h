#pragma once
#include <ztmedia/MediaTypes.h>
#include <ztmedia/SharedFrames.h>
#include <zto/PluginServices.h>
#include <ztprotocol/BulkChannel.h>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <map>

namespace vusiif::zto::media {
class MediaSession final : public QObject {
  public:
    std::function<void(const EncodedPacket &)> packet;
    std::function<void(const FrameView &)> frame;
    std::function<void(const MediaError &)> error;
    // Receiving track needs a fresh baseline/key frame after loss. The
    // application forwards this request to its corresponding sender.
    std::function<void(QString)> keyFrameRequired;
    std::function<void()> sourcesChanged;
    explicit MediaSession(std::shared_ptr<util::ServiceLease> lease, QObject *parent = nullptr)
        : QObject(parent), m_lease(std::move(lease)) {
        if (m_lease)
            m_lease->subscribe(this, [this](const util::ServiceResult &r) {
                if (!r.ok()) {
                    report({"service", r.error});
                    return;
                }
                auto event = r.data["event"].toString();
                if (event == "sources" && sourcesChanged)
                    sourcesChanged();
                else if (event == "error" || event == "channelError")
                    report({r.data["code"].toString("service"), r.data["message"].toString()});
            });
    }
    ~MediaSession() override {
        m_channel.close();
        if (m_lease)
            m_lease->close(nullptr, {});
    }
    quint64 generation() const { return m_lease ? m_lease->generation() : 0; }
    void capabilities(QObject *ctx, Completion<Capabilities> done) {
        call("capabilities", {}, ctx, [done](const util::ServiceResult &r) {
            done({{r.data["compiled"].toObject(), r.data["runtime"].toObject(), r.data["limits"].toObject()},
                  failure(r)});
        });
    }
    void sources(QObject *ctx, Completion<SourceSnapshot> done) {
        call("sources", {}, ctx, [done](const util::ServiceResult &r) {
            SourceSnapshot s;
            s.ready = r.ok();
            for (const auto &v : r.data["sources"].toArray()) {
                auto o = v.toObject();
                s.sources.append({o["id"].toString(), o["kind"].toString(), o["description"].toString(),
                                  o["default"].toBool()});
            }
            done({s, failure(r)});
        });
    }
    void open(QObject *ctx, Completion<bool> done) {
        if (m_channel.ready()) {
            QTimer::singleShot(0, ctx, [done] { done({true, {}}); });
            return;
        }
        call("$channel", {}, ctx, [this, scope = QPointer<QObject>(ctx), done](const util::ServiceResult &r) {
            if (!r.ok()) {
                done({false, failure(r)});
                return;
            }
            auto finished = std::make_shared<bool>(false);
            m_channel.connected = [scope, done, finished] {
                if (!*finished) {
                    *finished = true;
                    if (scope)
                        done({true, {}});
                }
            };
            m_channel.failed = [this, scope, done, finished](const QString &s) {
                QPointer<MediaSession> alive(this);
                auto dispatch = m_lease ? m_lease->callbackGuard() : std::shared_ptr<void>{};
                m_failed = true;
                if (!*finished) {
                    *finished = true;
                    if (scope)
                        done({false, {"channel", s}});
                }
                if (!alive)
                    return;
                if (alive && m_lease)
                    m_lease->close(nullptr, {});
                if (alive)
                    report({"channel", s});
            };
            m_channel.received = [this](const QJsonObject &h, const QByteArray &bytes) { receive(h, bytes); };
            m_channel.open(r.data);
        });
    }
    void createTrack(const TrackConfig &config, QObject *ctx, Completion<TrackDescriptor> done) {
        call("track.create", {{"config", toJson(config)}}, ctx, [this, done](const util::ServiceResult &r) {
            if (r.ok())
                m_tracks.insert(r.data["id"].toString());
            if (r.ok())
                m_configurations[r.data["id"].toString()] = r.data["configuration"].toString().toULongLong();
            done({{r.data["id"].toString(), generation(), r.data["configuration"].toString().toULongLong(),
                   trackConfig(r.data["config"].toObject())},
                  failure(r)});
        });
    }
    void configure(const QString &id, const TrackConfig &config, QObject *ctx,
                   Completion<TrackDescriptor> done) {
        call("track.configure", {{"id", id}, {"config", toJson(config)}}, ctx,
             [this, done](const util::ServiceResult &r) {
                 if (r.ok()) {
                     auto id = r.data["id"].toString();
                     clearFrames(id);
                     m_configurations[id] = r.data["configuration"].toString().toULongLong();
                 }
                 done({{r.data["id"].toString(), generation(),
                        r.data["configuration"].toString().toULongLong(),
                        trackConfig(r.data["config"].toObject())},
                       failure(r)});
             });
    }
    void start(const QString &id, QObject *ctx, Completion<bool> done = {}) {
        m_active.insert(id);
        action("track.start", id, ctx, std::move(done));
    }
    void stop(const QString &id, QObject *ctx, Completion<bool> done = {}) {
        m_active.remove(id);
        action("track.stop", id, ctx, std::move(done));
    }
    void remove(const QString &id, QObject *ctx, Completion<bool> done = {}) {
        m_active.remove(id);
        m_tracks.remove(id);
        m_configurations.remove(id);
        clearFrames(id);
        action("track.remove", id, ctx, std::move(done));
    }
    // Video send tracks only. With a running sender and a cached capture,
    // refreshes that canvas as an IDR even if the source has stopped producing
    // frames. Refreshes are bounded to the configured frame interval. Without
    // a cached capture, marks the next input frame as a key frame. Acceptance
    // does not guarantee packet delivery or prove the capture source is alive.
    void requestKeyFrame(const QString &id, QObject *ctx, Completion<bool> done = {}) {
        action("track.keyframe", id, ctx, std::move(done));
    }
    void setGain(const QString &id, float gain, bool muted, QObject *ctx) {
        call("track.gain", {{"id", id}, {"gain", double(gain)}, {"muted", muted}}, ctx,
             [this](const util::ServiceResult &r) {
                 if (!r.ok())
                     report(failure(r));
             });
    }
    bool submit(EncodedPacket p) {
        if (m_closing || m_failed || p.generation != generation())
            return false;
        auto h = toJson(p);
        h["type"] = "packet";
        return m_channel.send(h, p.data);
    }
    bool submitTo(const TrackDescriptor &target, EncodedPacket p) {
        p.track = target.id;
        p.generation = target.generation;
        p.configuration = target.configuration;
        return submit(std::move(p));
    }
    void statistics(QObject *ctx, Completion<Statistics> done) {
        call("statistics", {}, ctx, [done](const util::ServiceResult &r) { done({{r.data}, failure(r)}); });
    }
    void close(QObject *ctx, Completion<bool> done) {
        m_closing = true;
        packet = {};
        frame = {};
        keyFrameRequired = {};
        sourcesChanged = {};
        m_channel.close();
        m_frames.clear();
        if (m_lease)
            m_lease->close(ctx, [done](const util::ServiceResult &r) {
                if (done)
                    done({r.ok(), failure(r)});
            });
        else if (done)
            QTimer::singleShot(0, ctx, [done] { done({true, {}}); });
    }

  private:
    std::shared_ptr<util::ServiceLease> m_lease;
    protocol::service::BulkChannel m_channel;
    std::map<QString, std::shared_ptr<detail::SharedFrames>> m_frames;
    std::map<QString, QString> m_frameTracks;
    QSet<QString> m_tracks, m_active;
    QMap<QString, quint64> m_configurations;
    bool m_closing = false;
    bool m_failed = false;
    void clearFrames(const QString &id) {
        for (auto it = m_frameTracks.begin(); it != m_frameTracks.end();)
            if (it->second == id) {
                m_frames.erase(it->first);
                it = m_frameTracks.erase(it);
            } else
                ++it;
    }
    static MediaError failure(const util::ServiceResult &r) {
        return r.ok() ? MediaError{} : MediaError{"service", r.error};
    }
    void report(const MediaError &e) {
        auto callback = error;
        if (!m_closing && callback)
            callback(e);
    }
    void call(const QString &method, const QJsonObject &data, QObject *ctx, util::ServiceCallback done) {
        if (m_lease && !m_closing && !m_failed)
            m_lease->call(method, data, {}, ctx, std::move(done));
        else
            QTimer::singleShot(0, ctx, [done] { done({{}, {}, "Media session unavailable"}); });
    }
    void action(const QString &method, const QString &id, QObject *ctx, Completion<bool> done) {
        call(method, {{"id", id}}, ctx, [this, method, id, done](const util::ServiceResult &r) {
            if (!r.ok() && method == "track.start")
                m_active.remove(id);
            if (done)
                done({r.ok(), failure(r)});
            else if (!r.ok())
                report(failure(r));
        });
    }
    void receive(const QJsonObject &h, const QByteArray &bytes) {
        auto dispatch = m_lease ? m_lease->callbackGuard() : std::shared_ptr<void>{};
        if (m_closing || h["generation"].toString().toULongLong() != generation())
            return;
        if (!m_tracks.contains(h["track"].toString()) || !m_active.contains(h["track"].toString()) ||
            h["configuration"].toString().toULongLong() != m_configurations.value(h["track"].toString())) {
            if (h["type"] == "frame") {
                detail::SharedFrames m;
                if (m.open(h["mapping"].toString(), generation()))
                    m.discard(h["slot"].toInt(-1), h["sequence"].toString().toULongLong());
            }
            return;
        }
        if (h["type"] == "keyframe-required") {
            auto callback = keyFrameRequired;
            if (callback) callback(h["track"].toString());
            return;
        }
        if (h["type"] == "packet") {
            auto callback = packet;
            if (callback)
                callback(encodedPacket(h, bytes));
            return;
        }
        if (h["type"] != "frame")
            return;
        const auto key = h["mapping"].toString();
        auto &mapping = m_frames[key];
        if (!mapping) {
            if (m_frames.size() > 128) {
                report({"limit", "Too many frame mappings"});
                return;
            }
            mapping = std::make_shared<detail::SharedFrames>();
            if (!mapping->open(key, generation())) {
                m_frames.erase(key);
                report({"frame", "Cannot map preview frame"});
                return;
            }
            m_frameTracks[key] = h["track"].toString();
        }
        const auto slot = h["slot"].toInt(-1);
        const auto size = h["size"].toInteger();
        const auto *data = mapping->acquire(slot, size, h["sequence"].toString().toULongLong());
        if (!data)
            return;
        struct Guard {
            std::shared_ptr<detail::SharedFrames> m;
            int slot;
            ~Guard() { m->release(slot); }
        } guard{mapping, slot};
        FrameView v;
        v.track = h["track"].toString();
        v.generation = generation();
        v.sequence = h["sequence"].toString().toULongLong();
        v.configuration = h["configuration"].toString().toULongLong();
        v.pts = h["pts"].toString().toLongLong();
        v.duration = h["duration"].toString().toLongLong();
        v.video.width = h["width"].toInt();
        v.video.height = h["height"].toInt();
        // Preview pixels are full-range BGRA, including H.264's RGB conversion.
        v.video.colorRange = QStringLiteral("full");
        v.decoded = h["decoded"].toBool();
        v.data = data;
        v.size = size;
        v.planes.append({0, size, h["stride"].toInt()});
        if (v.video.width < 1 || v.video.height < 1 || v.video.width > 1920 || v.video.height > 1080 ||
            v.planes[0].stride < v.video.width * 4 || qint64(v.planes[0].stride) * v.video.height > size)
            return;
        auto callback = frame;
        if (callback)
            callback(v);
    }
};
} // namespace vusiif::zto::media
