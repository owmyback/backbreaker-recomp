// One-XMA-frame decoder (issue #93): FFmpeg's "xmaframes" decoder (WMA Pro, one frame per
// packet), built by scripts/build_xma_decoder.sh into third_party/ffmpeg-xma. The calls follow
// Xenia's XmaContextNew::PrepareDecoder / DecodePacket and XmaContext::ConvertFrame
// (xenia-canary src/xenia/apu/xma_context_new.cc, xma_context.cc; BSD 3-clause, see
// docs/licenses/xenia-LICENSE.txt). Without BB_XMA_DECODER this file only has stubs.
#include "xma_frame_decoder.h"

#ifdef BB_XMA_DECODER
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace apu::xma
{
    struct FrameDecoder
    {
        const AVCodec* codec = nullptr;
        AVCodecContext* ctx = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        int rate = 0, channels = 0;
        char error[160] = "";
    };

    static const AVCodec* findCodec() { return avcodec_find_decoder_by_name("xmaframes"); }

    bool decoderAvailable() { return findCodec() != nullptr; }

    FrameDecoder* createFrameDecoder()
    {
        const AVCodec* codec = findCodec();
        if (!codec)
            return nullptr;
        auto* d = new FrameDecoder;
        d->codec = codec;
        d->packet = av_packet_alloc();
        d->frame = av_frame_alloc();
        return d;
    }

    void destroyFrameDecoder(FrameDecoder* d)
    {
        if (!d) return;
        avcodec_free_context(&d->ctx);
        av_frame_free(&d->frame);
        av_packet_free(&d->packet);
        delete d;
    }

    static void setError(FrameDecoder* d, const char* what, int ret)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] = "";
        av_strerror(ret, buf, sizeof(buf));
        std::snprintf(d->error, sizeof(d->error), "%s: %s (%d)", what, buf, ret);
    }

    // A new context per stream, sample rate or channel count: the decoder keeps the previous
    // frame's overlap, which must not leak into another stream.
    static bool open(FrameDecoder* d, int rate, int channels)
    {
        avcodec_free_context(&d->ctx);
        d->ctx = avcodec_alloc_context3(d->codec);
        if (!d->ctx)
        {
            std::snprintf(d->error, sizeof(d->error), "avcodec_alloc_context3 failed");
            return false;
        }
        d->ctx->sample_rate = rate;
        av_channel_layout_default(&d->ctx->ch_layout, channels);
        d->ctx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL; // trim side data is ignored, as in Xenia
        if (int ret = avcodec_open2(d->ctx, d->codec, nullptr); ret < 0)
        {
            setError(d, "avcodec_open2", ret);
            avcodec_free_context(&d->ctx);
            return false;
        }
        d->rate = rate;
        d->channels = channels;
        return true;
    }

    bool decodeFrame(FrameDecoder* d, int sampleRate, int channels, bool reset,
                     const uint8_t* packet, int size, uint8_t* out)
    {
        if (!d)
            return false;
        if ((reset || !d->ctx || d->rate != sampleRate || d->channels != channels) && !open(d, sampleRate, channels))
            return false;
        d->packet->data = const_cast<uint8_t*>(packet); // not ref-counted: send_packet copies it
        d->packet->size = size;
        if (int ret = avcodec_send_packet(d->ctx, d->packet); ret < 0)
        {
            setError(d, "send_packet", ret);
            return false;
        }
        if (int ret = avcodec_receive_frame(d->ctx, d->frame); ret < 0)
        {
            setError(d, "receive_frame", ret);
            return false;
        }
        const int n = std::min<int>(d->frame->nb_samples, 512);
        const auto* left = reinterpret_cast<const float*>(d->frame->data[0]);
        const auto* right = channels == 2 && d->frame->data[1] ? reinterpret_cast<const float*>(d->frame->data[1]) : nullptr;
        // Saturate: the decoder's output is not limited to [-1, 1] (Xenia ConvertFrame).
        auto put = [](uint8_t* p, float s) {
            auto v = int16_t(std::lrintf(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
            p[0] = uint8_t(uint16_t(v) >> 8);
            p[1] = uint8_t(v);
        };
        const int stride = channels == 2 ? 4 : 2;
        for (int i = 0; i < 512; i++)
        {
            put(out + i * stride, i < n ? left[i] : 0.0f);
            if (channels == 2)
                put(out + i * stride + 2, i < n ? (right ? right[i] : left[i]) : 0.0f);
        }
        av_frame_unref(d->frame);
        return true;
    }

    const char* lastError(FrameDecoder* d) { return d ? d->error : "no decoder"; }
}

#else // no decoder built in: XMA stays silent

namespace apu::xma
{
    bool decoderAvailable() { return false; }
    FrameDecoder* createFrameDecoder() { return nullptr; }
    void destroyFrameDecoder(FrameDecoder*) {}
    bool decodeFrame(FrameDecoder*, int, int, bool, const uint8_t*, int, uint8_t*) { return false; }
    const char* lastError(FrameDecoder*) { return "no decoder in this build"; }
}

#endif
