#include <ztmediaengine/Processor.h>
#include <ztmediaengine/Fingerprint.h>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}
#include <cstdio>
namespace {
struct Flip {
    AVFilterGraph *graph = nullptr;
    AVFilterContext *source = nullptr, *sink = nullptr;
    int width = 0, height = 0, format = -1;
    ~Flip() { avfilter_graph_free(&graph); }
};
void *create(const char *, char *, uint32_t) {
    try {
        return new Flip;
    } catch (...) {
        return nullptr;
    }
}
int process(void *instance, const AVFrame *input, ZTMediaEmitFrame emit, void *context, char *error,
            uint32_t capacity) {
    auto *f = static_cast<Flip *>(instance);
    if (!f || !input || !emit)
        return -1;
    if (!f->graph || f->width != input->width || f->height != input->height || f->format != input->format) {
        avfilter_graph_free(&f->graph);
        f->graph = avfilter_graph_alloc();
        if (!f->graph)
            return -1;
        char arguments[256];
        std::snprintf(arguments, sizeof arguments,
                      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", input->width,
                      input->height, input->format, input->time_base.num > 0 ? input->time_base.num : 1,
                      input->time_base.den > 0 ? input->time_base.den : 1000000);
        AVFilterContext *flip = nullptr;
        int status = avfilter_graph_create_filter(&f->source, avfilter_get_by_name("buffer"), "input",
                                                  arguments, nullptr, f->graph);
        if (status >= 0)
            status = avfilter_graph_create_filter(&flip, avfilter_get_by_name("hflip"), "flip", nullptr,
                                                  nullptr, f->graph);
        if (status >= 0)
            status = avfilter_graph_create_filter(&f->sink, avfilter_get_by_name("buffersink"), "output",
                                                  nullptr, nullptr, f->graph);
        if (status >= 0)
            status = avfilter_link(f->source, 0, flip, 0);
        if (status >= 0)
            status = avfilter_link(flip, 0, f->sink, 0);
        if (status >= 0)
            status = avfilter_graph_config(f->graph, nullptr);
        if (status < 0) {
            std::snprintf(error, capacity, "Cannot create hflip filter graph (%d)", status);
            return status;
        }
        f->width = input->width;
        f->height = input->height;
        f->format = input->format;
    }
    int status =
        av_buffersrc_add_frame_flags(f->source, const_cast<AVFrame *>(input), AV_BUFFERSRC_FLAG_KEEP_REF);
    if (status < 0)
        return status;
    auto *output = av_frame_alloc();
    if (!output)
        return -1;
    while ((status = av_buffersink_get_frame(f->sink, output)) >= 0) {
        emit(context, output);
        av_frame_unref(output);
    }
    av_frame_free(&output);
    return status == AVERROR(EAGAIN) || status == AVERROR_EOF ? 0 : status;
}
int drain(void *, ZTMediaEmitFrame, void *, char *, uint32_t) { return 0; }
void destroy(void *instance) { delete static_cast<Flip *>(instance); }
const ZTMediaProcessor processors[] = {
    {sizeof(ZTMediaProcessor), "example.hflip", "video", create, process, drain, destroy}};
const ZTMediaExtension extension{sizeof(ZTMediaExtension), ZTMEDIA_ENGINE_ABI, ZTMEDIA_FFMPEG_FINGERPRINT, 1,
                                 processors};
} // namespace
extern "C" ZTMEDIA_EXPORT const ZTMediaExtension *ztmedia_extension_v1() { return &extension; }
