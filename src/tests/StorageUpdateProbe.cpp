#include "plugins/HostServicesImpl.h"
#include "../plugins/voice-chat/client/VoiceConfig.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
using namespace vusiif::ztgui;
using util::PluginFileSnapshot;
using util::PluginFileUpdate;

// Two independent processes touch the same file with updateFile(). The host
// must serialize them and preserve both fields instead of replacing the file
// with a stale whole-file snapshot.
int updateJsonField(const QString &dataRoot, const QString &file, const QString &key,
                    const QString &value, int iterations)
{
    util::HostServicesImpl host(QStringLiteral("FieldProbe"), dataRoot,
                                QStringLiteral("field-probe"));
    if (!host.storageReady()) return 3;
    auto *storage = dynamic_cast<util::PluginStorage *>(&host.files());
    if (!storage) return 4;
    for (int index = 0; index < iterations; ++index) {
        const auto result = storage->updateFile(file, [&](const PluginFileSnapshot &snapshot) {
            QJsonObject object;
            if (snapshot.exists && !snapshot.data.isEmpty())
                object = QJsonDocument::fromJson(snapshot.data).object();
            bool numeric = false;
            const int number = value.toInt(&numeric);
            object[key] = numeric ? QJsonValue(number) : QJsonValue(value);
            return PluginFileUpdate::replace(QJsonDocument(object).toJson(QJsonDocument::Compact));
        });
        if (!result.ok) return 5;
    }
    return 0;
}

// Mirrors the voice plugin's per-member merge: each process only adds its own
// ZeroTier ID, so two users editing different members must not lose an entry.
int updateMemberRemarks(const QString &dataRoot, const QString &ztid, const QString &remark,
                        int iterations)
{
    util::HostServicesImpl host(QStringLiteral("RemarkProbe"), dataRoot,
                                QStringLiteral("remark-probe"));
    if (!host.storageReady()) return 3;
    auto *storage = dynamic_cast<util::PluginStorage *>(&host.files());
    if (!storage) return 4;
    for (int index = 0; index < iterations; ++index) {
        QJsonObject updated;
        QString error;
        if (!voice_chat::updateMemberRemark(*storage, ztid, remark, updated, error)) return 5;
    }
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto argument = [&](int index) {
        return index < argc ? QString::fromLocal8Bit(argv[index]) : QString();
    };
    if (argc >= 2 && argument(1) == QLatin1String("--json-field")) {
        if (argc < 6) return 2;
        const int iterations = argc > 6 ? argument(6).toInt() : 1;
        if (iterations < 1) return 2;
        return updateJsonField(argument(2), argument(3), argument(4), argument(5), iterations);
    }
    if (argc >= 2 && argument(1) == QLatin1String("--remark")) {
        if (argc < 5) return 2;
        const int iterations = argc > 5 ? argument(5).toInt() : 1;
        if (iterations < 1) return 2;
        return updateMemberRemarks(argument(2), argument(3), argument(4), iterations);
    }
    if (argc < 3) return 2;
    const QString dataRoot = argument(1);
    const int iterations = argument(2).toInt();
    util::HostServicesImpl host(QStringLiteral("StorageProbe"), dataRoot,
                                QStringLiteral("storage-probe"));
    if (!host.storageReady()) return 3;
    auto *storage = dynamic_cast<util::PluginStorage *>(&host.files());
    if (!storage) return 4;
    for (int index = 0; index < iterations; ++index) {
        const auto result = storage->updateFile("counter.json", [](const PluginFileSnapshot &snapshot) {
            const int current = snapshot.exists ? snapshot.data.toInt() : 0;
            return PluginFileUpdate::replace(QByteArray::number(current + 1));
        });
        if (!result.ok) return 5;
    }
    return 0;
}
