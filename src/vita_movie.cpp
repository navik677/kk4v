// Minimal movie (.mpg/.mp4/etc) playback for the Vita port, using ffmpeg
// (already available prebuilt in vitasdk) to implement the engine's
// iTVPVideoOverlay interface (krmovie.h). This is intentionally simple:
// Play() runs a synchronous decode+present loop on the calling thread
// (video only; movie audio is not decoded here -- most KAG games already
// play their own BGM/voice tracks independently of the opening movie).
// A tap/button press skips the movie early.

#include "tjsCommHead.h"
#include "krmovie.h"
#include "combase.h"
#include "WindowIntf.h"
#include "VideoOvlImpl.h"
#include "vita_klog.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <SDL2/SDL.h>
#include <psp2/ctrl.h>
#include <string>
#include <cstdio>

extern SDL_Renderer* s_SDLRenderer;

namespace {

int AVIOReadFunc(void *opaque, uint8_t *buf, int buf_size) {
    IStream *stream = (IStream *)opaque;
    ULONG read = 0;
    HRESULT hr = stream->Read(buf, (ULONG)buf_size, &read);
    if (FAILED(hr) || read == 0) return AVERROR_EOF;
    return (int)read;
}

int64_t AVIOSeekFunc(void *opaque, int64_t offset, int whence) {
    IStream *stream = (IStream *)opaque;
    if (whence == AVSEEK_SIZE) {
        LARGE_INTEGER zero; zero.QuadPart = 0;
        ULARGE_INTEGER cur;
        stream->Seek(zero, STREAM_SEEK_CUR, &cur);
        LARGE_INTEGER end; end.QuadPart = 0;
        ULARGE_INTEGER newpos;
        stream->Seek(end, STREAM_SEEK_END, &newpos);
        LARGE_INTEGER back; back.QuadPart = (int64_t)cur.QuadPart;
        ULARGE_INTEGER restored;
        stream->Seek(back, STREAM_SEEK_SET, &restored);
        return (int64_t)newpos.QuadPart;
    }
    DWORD origin = STREAM_SEEK_SET;
    if (whence == SEEK_CUR) origin = STREAM_SEEK_CUR;
    else if (whence == SEEK_END) origin = STREAM_SEEK_END;
    LARGE_INTEGER move; move.QuadPart = offset;
    ULARGE_INTEGER newpos;
    HRESULT hr = stream->Seek(move, origin, &newpos);
    if (FAILED(hr)) return -1;
    return (int64_t)newpos.QuadPart;
}

} // namespace

class VitaVideoOverlay : public iTVPVideoOverlay {
public:
    VitaVideoOverlay() {}
    ~VitaVideoOverlay() { Close(); }

    void AddRef() override { RefCount++; }
    void Release() override { if (--RefCount <= 0) delete this; }

    void BuildGraph(tTJSNI_VideoOverlay *callbackwin, IStream *stream,
        const tjs_char *streamname, const tjs_char *type, uint64_t size) {
        Stream = stream;
        Stream->AddRef();
        char logMsg[300];
        std::string narrow = ttstr(streamname).AsNarrowStdString();
        snprintf(logMsg, sizeof(logMsg), "[KK4V] Movie: BuildGraph %s (%llu bytes)",
            narrow.c_str(), (unsigned long long)size);
        KK4V_Log(logMsg);
        OpenStream();
    }

    void SetWindow(tTJSNI_Window *window) override {}
    void SetMessageDrainWindow(void *window) override {}
    void SetRect(int l, int t, int r, int b) override {
        RectL = l; RectT = t; RectR = r; RectB = b;
    }
    void SetVisible(bool b) override { Visible = b; }

    void Play() override {
        if (!FormatCtx || VideoStreamIdx < 0) { Status = vsStopped; return; }
        Status = vsPlaying;
        KK4V_Log("[KK4V] Movie: Play() starting decode loop");
        DecodeLoop();
        Status = vsStopped;
        KK4V_Log("[KK4V] Movie: Play() loop finished");
    }
    void Stop() override { StopRequested = true; Status = vsStopped; }
    void Pause() override { Status = vsPaused; }
    void SetPosition(uint64_t tick) override {}
    void GetPosition(uint64_t *tick) override { if (tick) *tick = 0; }
    void GetStatus(tTVPVideoStatus *status) override { if (status) *status = Status; }

    void Rewind() override {
        if (FormatCtx) av_seek_frame(FormatCtx, VideoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
    }
    void SetFrame(int f) override {}
    void GetFrame(int *f) override { if (f) *f = CurFrame; }
    void GetFPS(double *f) override { if (f) *f = Fps > 0 ? Fps : 30.0; }
    void GetNumberOfFrame(int *f) override { if (f) *f = 0; }
    void GetTotalTime(int64_t *t) override { if (t) *t = 0; }

    void GetVideoSize(long *width, long *height) override {
        if (width) *width = VideoWidth;
        if (height) *height = VideoHeight;
    }
    tTVPBaseTexture* GetFrontBuffer() override { return nullptr; }
    void SetVideoBuffer(tTVPBaseTexture *buff1, tTVPBaseTexture *buff2, long size) override {}

    void SetStopFrame(int frame) override {}
    void GetStopFrame(int *frame) override { if (frame) *frame = -1; }
    void SetDefaultStopFrame() override {}

    void SetPlayRate(double rate) override {}
    void GetPlayRate(double *rate) override { if (rate) *rate = 1.0; }

    void SetAudioBalance(long balance) override {}
    void GetAudioBalance(long *balance) override { if (balance) *balance = 0; }
    void SetAudioVolume(long volume) override {}
    void GetAudioVolume(long *volume) override { if (volume) *volume = 0; }

    void GetNumberOfAudioStream(unsigned long *streamCount) override { if (streamCount) *streamCount = 0; }
    void SelectAudioStream(unsigned long num) override {}
    void GetEnableAudioStreamNum(long *num) override { if (num) *num = -1; }
    void DisableAudioStream() override {}

    void GetNumberOfVideoStream(unsigned long *streamCount) override { if (streamCount) *streamCount = VideoStreamIdx >= 0 ? 1 : 0; }
    void SelectVideoStream(unsigned long num) override {}
    void GetEnableVideoStreamNum(long *num) override { if (num) *num = VideoStreamIdx; }

    void SetMixingBitmap(tTVPBaseTexture *dest, float alpha) override {}
    void ResetMixingBitmap() override {}
    void SetMixingMovieAlpha(float a) override {}
    void GetMixingMovieAlpha(float *a) override { if (a) *a = 1.0f; }
    void SetMixingMovieBGColor(unsigned long col) override {}
    void GetMixingMovieBGColor(unsigned long *col) override { if (col) *col = 0; }

    void PresentVideoImage() override {}

    void GetContrastRangeMin(float *v) override { if (v) *v = 0; }
    void GetContrastRangeMax(float *v) override { if (v) *v = 0; }
    void GetContrastDefaultValue(float *v) override { if (v) *v = 0; }
    void GetContrastStepSize(float *v) override { if (v) *v = 0; }
    void GetContrast(float *v) override { if (v) *v = 0; }
    void SetContrast(float v) override {}

    void GetBrightnessRangeMin(float *v) override { if (v) *v = 0; }
    void GetBrightnessRangeMax(float *v) override { if (v) *v = 0; }
    void GetBrightnessDefaultValue(float *v) override { if (v) *v = 0; }
    void GetBrightnessStepSize(float *v) override { if (v) *v = 0; }
    void GetBrightness(float *v) override { if (v) *v = 0; }
    void SetBrightness(float v) override {}

    void GetHueRangeMin(float *v) override { if (v) *v = 0; }
    void GetHueRangeMax(float *v) override { if (v) *v = 0; }
    void GetHueDefaultValue(float *v) override { if (v) *v = 0; }
    void GetHueStepSize(float *v) override { if (v) *v = 0; }
    void GetHue(float *v) override { if (v) *v = 0; }
    void SetHue(float v) override {}

    void GetSaturationRangeMin(float *v) override { if (v) *v = 0; }
    void GetSaturationRangeMax(float *v) override { if (v) *v = 0; }
    void GetSaturationDefaultValue(float *v) override { if (v) *v = 0; }
    void GetSaturationStepSize(float *v) override { if (v) *v = 0; }
    void GetSaturation(float *v) override { if (v) *v = 0; }
    void SetSaturation(float v) override {}

    void SetLoopSegement(int beginFrame, int endFrame) override {}

private:
    int RefCount = 1;
    IStream *Stream = nullptr;
    AVIOContext *IOCtx = nullptr;
    AVFormatContext *FormatCtx = nullptr;
    AVCodecContext *VideoCodecCtx = nullptr;
    SwsContext *SwsCtx = nullptr;
    int VideoStreamIdx = -1;
    long VideoWidth = 0, VideoHeight = 0;
    double Fps = 30.0;
    int CurFrame = 0;
    bool Visible = true;
    bool StopRequested = false;
    int RectL = 0, RectT = 0, RectR = 960, RectB = 544;
    tTVPVideoStatus Status = vsStopped;

    void Close() {
        if (VideoCodecCtx) { avcodec_free_context(&VideoCodecCtx); VideoCodecCtx = nullptr; }
        if (SwsCtx) { sws_freeContext(SwsCtx); SwsCtx = nullptr; }
        if (FormatCtx) {
            if (IOCtx) FormatCtx->pb = nullptr;
            avformat_close_input(&FormatCtx);
        }
        if (IOCtx) {
            if (IOCtx->buffer) av_free(IOCtx->buffer);
            avio_context_free(&IOCtx);
        }
        if (Stream) { Stream->Release(); Stream = nullptr; }
    }

    void OpenStream() {
        const int bufSize = 64 * 1024;
        uint8_t *buf = (uint8_t *)av_malloc(bufSize + AV_INPUT_BUFFER_PADDING_SIZE);
        IOCtx = avio_alloc_context(buf, bufSize, 0, Stream, AVIOReadFunc, nullptr, AVIOSeekFunc);
        if (!IOCtx) { KK4V_Log("[KK4V] Movie: avio_alloc_context failed"); return; }

        FormatCtx = avformat_alloc_context();
        FormatCtx->pb = IOCtx;

        if (avformat_open_input(&FormatCtx, "", nullptr, nullptr) != 0) {
            KK4V_Log("[KK4V] Movie: avformat_open_input failed");
            FormatCtx = nullptr;
            return;
        }
        if (avformat_find_stream_info(FormatCtx, nullptr) < 0) {
            KK4V_Log("[KK4V] Movie: avformat_find_stream_info failed");
            return;
        }

        VideoStreamIdx = av_find_best_stream(FormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (VideoStreamIdx < 0) {
            KK4V_Log("[KK4V] Movie: no video stream found");
            return;
        }

        AVStream *vs = FormatCtx->streams[VideoStreamIdx];
        const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) { KK4V_Log("[KK4V] Movie: no decoder for codec"); VideoStreamIdx = -1; return; }

        VideoCodecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(VideoCodecCtx, vs->codecpar);
        if (avcodec_open2(VideoCodecCtx, codec, nullptr) < 0) {
            KK4V_Log("[KK4V] Movie: avcodec_open2 failed");
            VideoStreamIdx = -1;
            return;
        }

        VideoWidth = VideoCodecCtx->width;
        VideoHeight = VideoCodecCtx->height;
        if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
            Fps = av_q2d(vs->avg_frame_rate);

        char logMsg[200];
        snprintf(logMsg, sizeof(logMsg), "[KK4V] Movie: opened %ldx%ld @ %.2ffps codec=%s",
            VideoWidth, VideoHeight, Fps, codec->name ? codec->name : "?");
        KK4V_Log(logMsg);
    }

    void DecodeLoop() {
        if (!FormatCtx || !VideoCodecCtx) return;

        SwsCtx = sws_getContext(VideoWidth, VideoHeight, VideoCodecCtx->pix_fmt,
            VideoWidth, VideoHeight, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!SwsCtx) { KK4V_Log("[KK4V] Movie: sws_getContext failed"); return; }

        SDL_Texture *tex = nullptr;
        if (s_SDLRenderer) {
            tex = SDL_CreateTexture(s_SDLRenderer, SDL_PIXELFORMAT_ABGR8888,
                SDL_TEXTUREACCESS_STREAMING, VideoWidth, VideoHeight);
        }

        uint8_t *rgbaBuf = (uint8_t *)av_malloc(VideoWidth * VideoHeight * 4);
        uint8_t *dstPlanes[4] = { rgbaBuf, nullptr, nullptr, nullptr };
        int dstStride[4] = { (int)(VideoWidth * 4), 0, 0, 0 };

        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        StopRequested = false;
        Uint32 startTicks = SDL_GetTicks();
        int frameIdx = 0;

        while (!StopRequested && av_read_frame(FormatCtx, pkt) >= 0) {
            if (pkt->stream_index == VideoStreamIdx) {
                if (avcodec_send_packet(VideoCodecCtx, pkt) == 0) {
                    while (avcodec_receive_frame(VideoCodecCtx, frame) == 0) {
                        sws_scale(SwsCtx, frame->data, frame->linesize, 0, VideoHeight,
                            dstPlanes, dstStride);

                        if (tex) {
                            SDL_UpdateTexture(tex, nullptr, rgbaBuf, VideoWidth * 4);
                            if (Visible) {
                                SDL_SetRenderDrawColor(s_SDLRenderer, 0, 0, 0, 255);
                                SDL_RenderClear(s_SDLRenderer);
                                SDL_Rect dst{ RectL, RectT, RectR - RectL, RectB - RectT };
                                SDL_RenderCopy(s_SDLRenderer, tex, nullptr, &dst);
                                SDL_RenderPresent(s_SDLRenderer);
                            }
                        }
                        CurFrame = frameIdx++;

                        // pace playback to the video's frame rate
                        double expectedMs = frameIdx * (1000.0 / (Fps > 0 ? Fps : 30.0));
                        Uint32 elapsed = SDL_GetTicks() - startTicks;
                        if (expectedMs > elapsed) SDL_Delay((Uint32)(expectedMs - elapsed));

                        // allow skipping the movie via any button/tap
                        SceCtrlData pad;
                        if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && pad.buttons != 0)
                            StopRequested = true;
                        SDL_Event ev;
                        while (SDL_PollEvent(&ev)) {
                            if (ev.type == SDL_QUIT || ev.type == SDL_MOUSEBUTTONDOWN)
                                StopRequested = true;
                        }
                        if (StopRequested) break;
                    }
                }
            }
            av_packet_unref(pkt);
            if (StopRequested) break;
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_free(rgbaBuf);
        if (tex) SDL_DestroyTexture(tex);
        KK4V_Log("[KK4V] Movie: decode loop exited");
    }
};

static bool s_FFmpegInited = false;
static void EnsureFFmpegInit() {
    if (!s_FFmpegInited) {
        av_log_set_level(AV_LOG_ERROR);
        s_FFmpegInited = true;
    }
}

void GetVideoOverlayObject(
    tTJSNI_VideoOverlay *callbackwin, IStream *stream, const tjs_char *streamname,
    const tjs_char *type, uint64_t size, iTVPVideoOverlay **out) {
    EnsureFFmpegInit();
    VitaVideoOverlay *ov = new VitaVideoOverlay();
    ov->BuildGraph(callbackwin, stream, streamname, type, size);
    *out = ov;
}
