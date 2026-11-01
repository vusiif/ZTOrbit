#pragma once
#include <ztmedia/MediaSession.h>
#include <QMap>
#include <QPointer>
#include <QQueue>
#include <set>
namespace vusiif::zto::voice_chat {
// Voice owns its room-to-track mapping. The media SDK has no member/room model.
class VoiceAudio final : public QObject {
  public:
    std::function<void(const QByteArray &)> encoded;
    std::function<void(const QString &)> notice;
    explicit VoiceAudio(media::MediaSession *session, QObject *parent = nullptr)
        : QObject(parent), session(session) {
        session->packet = [this](const media::EncodedPacket &p) {
            if (active && p.track == sender && encoded)
                encoded(p.data);
        };
        session->error = [this](const media::MediaError &e) {
            if (notice)
                notice(e.message);
        };
    }
    ~VoiceAudio() override {
        session->packet = {};
        session->error = {};
    }
    void start(const QByteArray &input, const QByteArray &output, bool recordAllowed) {
        stop();
        active = true;
        inputId = input;
        outputId = output;
        allowed = recordAllowed;
        auto revision = epoch;
        if (opened) {
            build(revision);
            return;
        }
        if (opening)
            return;
        opening = true;
        session->open(this, [this](media::Result<bool> result) {
            opening = false;
            if (!result.ok()) {
                tell(result.error);
                return;
            }
            opened = true;
            if (active)
                build(epoch);
        });
    }
    void stop() {
        active = false;
        ++epoch;
        if (!sender.isEmpty())
            session->remove(sender, this);
        sender.clear();
        for (const auto &track : receivers)
            session->remove(track, this);
        receivers.clear();
        creating.clear();
        pending.clear();
    }
    void retain(const std::set<quint32> &ids) {
        members = ids;
        for (auto it = pending.begin(); it != pending.end();)
            if (!members.count(it.key()))
                it = pending.erase(it);
            else
                ++it;
        for (auto it = volumes.begin(); it != volumes.end();)
            if (!members.count(it.key()))
                it = volumes.erase(it);
            else
                ++it;
        for (auto i = receivers.begin(); i != receivers.end();)
            if (!members.count(i.key())) {
                session->remove(i.value(), this);
                volumes.remove(i.key());
                pending.remove(i.key());
                i = receivers.erase(i);
            } else
                ++i;
        if (active && opened)
            for (auto id : members)
                ensureReceiver(id);
    }
    void receive(quint32 id, quint32 sequence, const QByteArray &bytes) {
        if (!active || !members.count(id))
            return;
        media::EncodedPacket packet;
        packet.codec = "opus";
        packet.sequence = sequence;
        packet.pts = qint64(sequence) * 20000;
        packet.generation = session->generation();
        packet.data = bytes;
        if (receivers.contains(id)) {
            packet.track = receivers[id];
            session->submit(std::move(packet));
        } else {
            auto &queue = pending[id];
            if (queue.size() >= 5)
                queue.dequeue();
            queue.enqueue(std::move(packet));
            if (opened)
                ensureReceiver(id);
        }
    }
    void setMuted(bool value) {
        muted = value;
        if (!sender.isEmpty())
            session->setGain(sender, inputGain, muted, this);
    }
    void setDeafened(bool value) {
        deafened = value;
        updateGains();
    }
    void setVolume(float value) {
        volume = value;
        updateGains();
    }
    void setInputVolume(float value) {
        inputGain = value;
        if (!sender.isEmpty())
            session->setGain(sender, inputGain, muted, this);
    }
    void setMemberVolume(quint32 id, float value) {
        volumes[id] = value;
        updateGains();
    }
    void clearMemberVolumes() {
        volumes.clear();
        updateGains();
    }

  private:
    media::MediaSession *session;
    bool active = false, opened = false, opening = false, allowed = true, muted = false, deafened = false;
    quint64 epoch = 0;
    QString sender;
    QByteArray inputId, outputId;
    float inputGain = 1, volume = .8f;
    std::set<quint32> members, creating;
    QMap<quint32, QString> receivers;
    QMap<quint32, float> volumes;
    QMap<quint32, QQueue<media::EncodedPacket>> pending;
    void tell(const media::MediaError &e) {
        if (notice)
            notice(e.message);
    }
    void build(quint64 revision) {
        if (!active || revision != epoch)
            return;
        if (allowed) {
            media::TrackConfig c;
            c.source = QString::fromLatin1(inputId.toHex());
            c.gain = inputGain;
            session->createTrack(c, this, [this, revision](media::Result<media::TrackDescriptor> r) {
                if (!r.ok()) {
                    tell(r.error);
                    return;
                }
                if (!active || revision != epoch) {
                    session->remove(r.value.id, this);
                    return;
                }
                sender = r.value.id;
                session->setGain(sender, inputGain, muted, this);
                session->start(sender, this);
            });
        }
        for (auto id : members)
            ensureReceiver(id);
    }
    void ensureReceiver(quint32 id) {
        if (receivers.contains(id) || creating.count(id))
            return;
        creating.insert(id);
        auto revision = epoch;
        media::TrackConfig c;
        c.direction = "receive";
        c.output = QString::fromLatin1(outputId.toHex());
        c.gain = volume * volumes.value(id, 1);
        session->createTrack(c, this, [this, id, revision](media::Result<media::TrackDescriptor> r) {
            if (revision == epoch)
                creating.erase(id);
            if (!r.ok()) {
                tell(r.error);
                return;
            }
            if (!active || revision != epoch || !members.count(id)) {
                session->remove(r.value.id, this);
                return;
            }
            receivers[id] = r.value.id;
            session->setGain(r.value.id, volume * volumes.value(id, 1), deafened, this);
            session->start(r.value.id, this, [this, id, revision](media::Result<bool> started) {
                if (!started.ok()) {
                    tell(started.error);
                    return;
                }
                if (revision != epoch || !receivers.contains(id))
                    return;
                auto queue = pending.take(id);
                while (!queue.isEmpty()) {
                    auto p = queue.dequeue();
                    p.track = receivers[id];
                    session->submit(std::move(p));
                }
            });
        });
    }
    void updateGains() {
        for (auto i = receivers.cbegin(); i != receivers.cend(); ++i)
            session->setGain(i.value(), volume * volumes.value(i.key(), 1), deafened, this);
    }
};
} // namespace vusiif::zto::voice_chat
