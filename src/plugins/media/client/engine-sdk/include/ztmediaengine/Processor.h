#pragma once
#include <stdint.h>
extern "C" {
#include <libavutil/frame.h>
}
// This ABI is worker-local. Never serialize these pointers or pass C++/Qt
// objects across it. All callbacks run synchronously on the pipeline thread.
// ABI 1 accepts zero or one output frame per input; outputs preserve negotiated
// format/block size. AVFrame::time_base describes the input timestamp units.
#define ZTMEDIA_ENGINE_ABI 1u
#if defined(_WIN32)
#define ZTMEDIA_EXPORT __declspec(dllexport)
#else
#define ZTMEDIA_EXPORT __attribute__((visibility("default")))
#endif
extern "C" {
typedef void (*ZTMediaEmitFrame)(void *context, const AVFrame *frame);
struct ZTMediaProcessor {
    uint32_t size;
    const char *id;
    const char *mediaKind;
    void *(*create)(const char *configuration, char *error, uint32_t capacity);
    int (*process)(void *instance, const AVFrame *input, ZTMediaEmitFrame emit, void *context, char *error,
                   uint32_t capacity);
    int (*drain)(void *instance, ZTMediaEmitFrame emit, void *context, char *error, uint32_t capacity);
    void (*destroy)(void *instance);
};
struct ZTMediaExtension {
    uint32_t size;
    uint32_t abi;
    const char *ffmpegFingerprint;
    uint32_t processorCount;
    const ZTMediaProcessor *processors;
};
typedef const ZTMediaExtension *(*ZTMediaExtensionEntry)();
}
