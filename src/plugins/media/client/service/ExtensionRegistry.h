#pragma once
#include <ztmediaengine/Processor.h>
#include <ztmediaengine/Fingerprint.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLibrary>
#include <QMap>
#include <memory>
#include <vector>
namespace vusiif::ztgui::media::engine {
class ExtensionRegistry {
  public:
    ~ExtensionRegistry() {
        for (auto it = libraries.rbegin(); it != libraries.rend(); ++it)
            (*it)->unload();
    }
    QString load(const QString &root) {
        QFile file(root + "/extensions.json");
        if (!file.open(QIODevice::ReadOnly))
            return "Missing authenticated extensions.json";
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray())
            return "Invalid extension manifest";
        for (const auto &value : document.array()) {
            auto descriptor = value.toObject();
            auto relative = descriptor["path"].toString();
            if (!relative.startsWith("extensions/") || relative.contains("..") || relative.contains('\\') ||
                !relative.endsWith(".dll"))
                return "Invalid extension path";
            if (descriptor["abi"].toInt() != ZTMEDIA_ENGINE_ABI ||
                descriptor["ffmpeg"].toString() != ZTMEDIA_FFMPEG_FINGERPRINT)
                return "Extension manifest ABI/fingerprint mismatch";
            auto library = std::make_unique<QLibrary>(QDir(root).filePath(relative));
            if (!library->load())
                return library->errorString();
            auto entry = reinterpret_cast<ZTMediaExtensionEntry>(library->resolve("ztmedia_extension_v1"));
            if (!entry)
                return "Extension entry not found";
            const auto *api = entry();
            if (!api || api->size != sizeof(ZTMediaExtension) || api->abi != ZTMEDIA_ENGINE_ABI ||
                !api->ffmpegFingerprint || QByteArray(api->ffmpegFingerprint) != ZTMEDIA_FFMPEG_FINGERPRINT ||
                api->processorCount > 64 || (api->processorCount && !api->processors))
                return "Extension ABI/build fingerprint mismatch";
            for (uint32_t i = 0; i < api->processorCount; ++i) {
                const auto *p = api->processors + i;
                if (p->size != sizeof(ZTMediaProcessor) || !p->id || !p->mediaKind || !p->create ||
                    !p->process || !p->drain || !p->destroy || processors.contains(p->id))
                    return "Invalid/duplicate processor descriptor";
                processors.insert(p->id, p);
            }
            libraries.push_back(std::move(library));
        }
        return {};
    }
    const ZTMediaProcessor *find(const QString &id) const { return processors.value(id, nullptr); }
    QStringList ids() const { return processors.keys(); }

  private:
    std::vector<std::unique_ptr<QLibrary>> libraries;
    QMap<QString, const ZTMediaProcessor *> processors;
};
class ProcessorChain {
  public:
    ~ProcessorChain() { clear(); }
    void clear() {
        for (auto &p : instances) {
            char error[512]{};
            p.api->drain(p.instance, [](void *, const AVFrame *) {}, nullptr, error, sizeof error);
            p.api->destroy(p.instance);
        }
        instances.clear();
    }
    QString create(const QStringList &ids, const QString &kind, const ExtensionRegistry &registry) {
        clear();
        for (const auto &id : ids) {
            const auto *api = registry.find(id);
            if (!api || QString::fromUtf8(api->mediaKind) != kind)
                return "Unsupported processor: " + id;
            char error[512]{};
            auto *instance = api->create("{}", error, sizeof error);
            if (!instance)
                return QString::fromUtf8(error);
            instances.push_back({api, instance});
        }
        return {};
    }
    AVFrame *process(const AVFrame *input, QString &error) {
        AVFrame *current = av_frame_clone(input);
        if (!current)
            return nullptr;
        for (auto &p : instances) {
            struct Output {
                AVFrame *frame = nullptr;
                int count = 0;
            } output;
            char message[512]{};
            int result = p.api->process(
                p.instance, current,
                [](void *ctx, const AVFrame *frame) {
                    auto *out = static_cast<Output *>(ctx);
                    if (++out->count == 1 && frame)
                        out->frame = av_frame_clone(frame);
                },
                &output, message, sizeof message);
            av_frame_free(&current);
            current = output.frame;
            if (result < 0 || output.count > 1) {
                error = output.count > 1 ? QString("Engine ABI 1 processors emit at most one frame per input")
                                         : QString::fromUtf8(message);
                av_frame_free(&current);
                return nullptr;
            }
            if (!current)
                return nullptr;
        }
        return current;
    }

  private:
    struct Instance {
        const ZTMediaProcessor *api;
        void *instance;
    };
    std::vector<Instance> instances;
};
} // namespace vusiif::ztgui::media::engine
