// Декодирование видео/аудио через FFmpeg, вывод звука через SDL2,
// синхронизация видео по звуку.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include "player.h"

#define FQ_MAX       48
#define AUDIO_RATE   48000
#define AUDIO_BPS    (AUDIO_RATE * 4)          // stereo s16
#define AUDIO_MAX_Q  (AUDIO_BPS * 1)           // держим не больше 1 c звука в очереди
#define AUDIO_LOW_Q  (AUDIO_BPS * 15 / 100)    // меньше 0.15 c — звук «голодает»

struct Player {
    AVFormatContext   *fmt;
    AVCodecContext    *vctx, *actx;
    SwrContext        *swr;
    struct SwsContext *sws;
    int                vs, as;
    AVRational         vtb, atb;
    double             start_off, duration;
    SDL_AudioDeviceID  adev;
    int                adev_samples;
    uint8_t           *abuf;
    int                abuf_size;

    SDL_Thread        *thread;
    SDL_mutex         *mtx;
    volatile int       quit;

    // очередь готовых кадров
    AVFrame           *q[FQ_MAX];
    double             qpts[FQ_MAX];
    int                qhead, qcount, q_soft, q_hard;

    // перемотка
    int                seek_req;
    double             seek_target, skip_until;
    int                eof;

    // часы
    int                paused, buffering, need_frame;
    double             pause_clock, buffer_clock;
    double             clock_base, clock_time;
    double             audio_end_pts;
};

static double ts_sec(int64_t ts, AVRational tb) {
    return ts == AV_NOPTS_VALUE ? NAN : ts * av_q2d(tb);
}

static void queue_clear_locked(Player *p) {
    while (p->qcount > 0) {
        av_frame_free(&p->q[p->qhead]);
        p->qhead = (p->qhead + 1) % FQ_MAX;
        p->qcount--;
    }
    p->qhead = 0;
}

static void queue_push(Player *p, AVFrame *f, double pts) {
    SDL_LockMutex(p->mtx);
    if (p->qcount >= p->q_hard) { // переполнение — выбрасываем самый старый
        av_frame_free(&p->q[p->qhead]);
        p->qhead = (p->qhead + 1) % FQ_MAX;
        p->qcount--;
    }
    int idx = (p->qhead + p->qcount) % FQ_MAX;
    p->q[idx] = f;
    p->qpts[idx] = pts;
    p->qcount++;
    SDL_UnlockMutex(p->mtx);
}

static void handle_video_frame(Player *p, AVFrame *fr) {
    double pts = ts_sec(fr->best_effort_timestamp, p->vtb) - p->start_off;
    if (isnan(pts)) pts = 0;
    if (pts < p->skip_until - 0.001) { av_frame_unref(fr); return; }

    AVFrame *out = av_frame_alloc();
    if (!out) { av_frame_unref(fr); return; }

    if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_YUVJ420P) {
        av_frame_move_ref(out, fr);
    } else {
        // 10-бит, 4:2:2, NV12 и прочее — переводим в YUV420P
        p->sws = sws_getCachedContext(p->sws, fr->width, fr->height, fr->format,
                                      fr->width, fr->height, AV_PIX_FMT_YUV420P,
                                      SWS_FAST_BILINEAR, NULL, NULL, NULL);
        out->format = AV_PIX_FMT_YUV420P;
        out->width  = fr->width;
        out->height = fr->height;
        if (!p->sws || av_frame_get_buffer(out, 64) < 0) {
            av_frame_free(&out); av_frame_unref(fr); return;
        }
        sws_scale(p->sws, (const uint8_t * const *)fr->data, fr->linesize, 0, fr->height,
                  out->data, out->linesize);
        av_frame_copy_props(out, fr);
        av_frame_unref(fr);
    }
    queue_push(p, out, pts);
}

static void handle_audio_frame(Player *p, AVFrame *fr) {
    if (!p->adev) { av_frame_unref(fr); return; }
    double dur = fr->nb_samples / (double)fr->sample_rate;
    double pts = ts_sec(fr->best_effort_timestamp, p->atb) - p->start_off;
    if (isnan(pts)) pts = p->audio_end_pts;
    if (pts + dur < p->skip_until) { av_frame_unref(fr); return; }

    int outn = swr_get_out_samples(p->swr, fr->nb_samples);
    int need = outn * 4;
    if (need > p->abuf_size) {
        av_free(p->abuf);
        p->abuf = av_malloc(need);
        p->abuf_size = p->abuf ? need : 0;
        if (!p->abuf) { av_frame_unref(fr); return; }
    }
    uint8_t *outp[1] = { p->abuf };
    int got = swr_convert(p->swr, outp, outn, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (got > 0) SDL_QueueAudio(p->adev, p->abuf, got * 4);

    SDL_LockMutex(p->mtx);
    p->audio_end_pts = pts + dur;
    SDL_UnlockMutex(p->mtx);
    av_frame_unref(fr);
}

static void decode_packet(Player *p, AVCodecContext *ctx, AVPacket *pkt, AVFrame *fr, bool video) {
    if (avcodec_send_packet(ctx, pkt) < 0 && pkt) return;
    while (!p->quit && avcodec_receive_frame(ctx, fr) == 0) {
        if (video) handle_video_frame(p, fr);
        else       handle_audio_frame(p, fr);
    }
}

static void do_seek(Player *p) {
    SDL_LockMutex(p->mtx);
    double t = p->seek_target;
    p->seek_req = 0;
    queue_clear_locked(p);
    SDL_UnlockMutex(p->mtx);

    if (p->duration > 0 && t > p->duration - 0.5) t = p->duration - 0.5;
    if (t < 0) t = 0;

    int64_t ts = (int64_t)((t + p->start_off) * AV_TIME_BASE);
    avformat_seek_file(p->fmt, -1, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(p->vctx);
    if (p->actx) avcodec_flush_buffers(p->actx);
    if (p->adev) {
        SDL_PauseAudioDevice(p->adev, 1);
        SDL_ClearQueuedAudio(p->adev);
    }

    SDL_LockMutex(p->mtx);
    p->skip_until    = t;
    p->audio_end_pts = t;
    p->buffer_clock  = t;
    p->pause_clock   = t;
    p->buffering     = 1;
    p->need_frame    = 1;
    p->eof           = 0;
    SDL_UnlockMutex(p->mtx);
}

static int decode_thread(void *arg) {
    Player *p = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *fr  = av_frame_alloc();

    while (!p->quit) {
        if (p->seek_req) { do_seek(p); continue; }
        if (p->eof) { SDL_Delay(10); continue; }

        // не убегаем слишком далеко вперёд
        SDL_LockMutex(p->mtx);
        int qc = p->qcount;
        SDL_UnlockMutex(p->mtx);
        Uint32 aq = p->adev ? SDL_GetQueuedAudioSize(p->adev) : 0;
        bool audio_low  = p->adev && aq < AUDIO_LOW_Q && !p->paused;
        bool audio_full = p->adev && aq > AUDIO_MAX_Q;
        bool wait = (qc >= p->q_soft && !(audio_low && qc < p->q_hard - 1)) ||
                    (audio_full && qc >= 2);
        if (wait) { SDL_Delay(2); continue; }

        int r = av_read_frame(p->fmt, pkt);
        if (r < 0) {
            // конец файла: дочитываем буферы декодеров
            decode_packet(p, p->vctx, NULL, fr, true);
            if (p->actx) decode_packet(p, p->actx, NULL, fr, false);
            SDL_LockMutex(p->mtx);
            p->eof = 1;
            SDL_UnlockMutex(p->mtx);
            continue;
        }
        if (pkt->stream_index == p->vs)
            decode_packet(p, p->vctx, pkt, fr, true);
        else if (pkt->stream_index == p->as && p->actx)
            decode_packet(p, p->actx, pkt, fr, false);
        av_packet_unref(pkt);
    }

    av_frame_free(&fr);
    av_packet_free(&pkt);
    return 0;
}

static AVCodecContext *open_codec(AVStream *st, int threads, char *err, size_t errlen) {
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    if (!c) {
        snprintf(err, errlen, "Unsupported codec: %s", avcodec_get_name(st->codecpar->codec_id));
        return NULL;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    if (!ctx) return NULL;
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (threads > 0) {
        ctx->thread_count = threads;
        ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        ctx->flags2      |= AV_CODEC_FLAG2_FAST;
    }
    if (avcodec_open2(ctx, c, NULL) < 0) {
        snprintf(err, errlen, "Cannot open codec: %s", c->name);
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static bool open_audio(Player *p) {
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    if (p->actx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&p->actx->ch_layout, p->actx->ch_layout.nb_channels);
    if (swr_alloc_set_opts2(&p->swr, &out_layout, AV_SAMPLE_FMT_S16, AUDIO_RATE,
                            &p->actx->ch_layout, p->actx->sample_fmt, p->actx->sample_rate,
                            0, NULL) < 0 || swr_init(p->swr) < 0)
        return false;

    SDL_AudioSpec want = {0}, have;
    want.freq     = AUDIO_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    p->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!p->adev) return false;
    p->adev_samples = have.samples;
    return true;
}

Player *player_open(const char *path, char *err, size_t errlen) {
    Player *p = calloc(1, sizeof(Player));
    if (!p) { snprintf(err, errlen, "Out of memory"); return NULL; }
    p->vs = p->as = -1;

    if (avformat_open_input(&p->fmt, path, NULL, NULL) < 0) {
        snprintf(err, errlen, "Cannot open file");
        goto fail;
    }
    if (avformat_find_stream_info(p->fmt, NULL) < 0) {
        snprintf(err, errlen, "Cannot read stream info");
        goto fail;
    }

    p->vs = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (p->vs < 0) { snprintf(err, errlen, "No video stream"); goto fail; }
    p->vctx = open_codec(p->fmt->streams[p->vs], 3, err, errlen);
    if (!p->vctx) goto fail;
    p->vtb = p->fmt->streams[p->vs]->time_base;

    p->as = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, p->vs, NULL, 0);
    if (p->as >= 0) {
        char aerr[128];
        p->actx = open_codec(p->fmt->streams[p->as], 1, aerr, sizeof(aerr));
        if (p->actx) {
            p->atb = p->fmt->streams[p->as]->time_base;
            if (!open_audio(p)) {
                avcodec_free_context(&p->actx);
                swr_free(&p->swr);
                p->as = -1;
            }
        } else {
            p->as = -1;
        }
    }

    p->start_off = p->fmt->start_time != AV_NOPTS_VALUE ? p->fmt->start_time / (double)AV_TIME_BASE : 0;
    p->duration  = p->fmt->duration > 0 ? p->fmt->duration / (double)AV_TIME_BASE : 0;

    // ограничиваем очередь по памяти (~160 МБ), 4K-кадр весит ~12 МБ
    double frame_bytes = (double)p->vctx->width * p->vctx->height * 1.5 + 1;
    int hard = (int)(160.0 * 1024 * 1024 / frame_bytes);
    if (hard > FQ_MAX) hard = FQ_MAX;
    if (hard < 4) hard = 4;
    p->q_hard = hard;
    p->q_soft = hard < 8 ? hard : 8;

    p->buffering  = 1;
    p->need_frame = 1;
    p->mtx = SDL_CreateMutex();
    p->thread = SDL_CreateThread(decode_thread, "decode", p);
    if (!p->mtx || !p->thread) { snprintf(err, errlen, "Cannot start decoder thread"); goto fail; }
    return p;

fail:
    player_close(p);
    return NULL;
}

void player_close(Player *p) {
    if (!p) return;
    p->quit = 1;
    if (p->thread) SDL_WaitThread(p->thread, NULL);
    if (p->adev) SDL_CloseAudioDevice(p->adev);
    if (p->mtx) {
        SDL_LockMutex(p->mtx);
        queue_clear_locked(p);
        SDL_UnlockMutex(p->mtx);
        SDL_DestroyMutex(p->mtx);
    }
    av_free(p->abuf);
    sws_freeContext(p->sws);
    swr_free(&p->swr);
    avcodec_free_context(&p->vctx);
    avcodec_free_context(&p->actx);
    avformat_close_input(&p->fmt);
    free(p);
}

double player_clock(Player *p) {
    SDL_LockMutex(p->mtx);
    double t = now_sec(), c;

    if (p->buffering) {
        // ждём, пока накопятся кадры и немного звука
        Uint32 aq = p->adev ? SDL_GetQueuedAudioSize(p->adev) : 0;
        bool v_ok = p->qcount >= 2 || p->eof;
        bool a_ok = !p->adev || aq >= AUDIO_BPS / 10 || p->eof || p->qcount >= p->q_soft;
        if (v_ok && a_ok) {
            p->buffering  = 0;
            p->clock_base = p->buffer_clock;
            p->clock_time = t;
            if (!p->paused && p->adev) SDL_PauseAudioDevice(p->adev, 0);
        }
    }

    if (p->paused)         c = p->pause_clock;
    else if (p->buffering) c = p->buffer_clock;
    else {
        c = p->clock_base + (t - p->clock_time);
        if (p->adev) {
            Uint32 aq = SDL_GetQueuedAudioSize(p->adev);
            if (aq > 0) {
                double a = p->audio_end_pts - aq / (double)AUDIO_BPS - p->adev_samples / (double)AUDIO_RATE;
                if (fabs(a - c) > 0.03) { p->clock_base = a; p->clock_time = t; c = a; }
            }
        }
    }
    SDL_UnlockMutex(p->mtx);
    return c;
}

AVFrame *player_take_frame(Player *p, double clock) {
    AVFrame *res = NULL;
    SDL_LockMutex(p->mtx);
    if (p->need_frame && p->qcount > 0) {
        // сразу показать первый кадр после открытия/перемотки
        res = p->q[p->qhead];
        p->qhead = (p->qhead + 1) % FQ_MAX;
        p->qcount--;
        p->need_frame = 0;
    } else {
        while (p->qcount > 0 && p->qpts[p->qhead] <= clock + 0.005) {
            if (res) av_frame_free(&res); // опаздываем — пропускаем кадр
            res = p->q[p->qhead];
            p->qhead = (p->qhead + 1) % FQ_MAX;
            p->qcount--;
        }
    }
    SDL_UnlockMutex(p->mtx);
    return res;
}

void player_set_pause(Player *p, bool pause) {
    if (!!pause == !!p->paused) return;
    double c = player_clock(p);
    SDL_LockMutex(p->mtx);
    if (pause) {
        p->pause_clock = c;
        p->paused = 1;
        if (p->adev) SDL_PauseAudioDevice(p->adev, 1);
    } else {
        p->paused = 0;
        p->clock_base = p->pause_clock;
        p->clock_time = now_sec();
        if (p->buffering) p->buffer_clock = p->pause_clock;
        else if (p->adev) SDL_PauseAudioDevice(p->adev, 0);
    }
    SDL_UnlockMutex(p->mtx);
}

bool player_paused(Player *p) { return p->paused; }

void player_seek(Player *p, double target) {
    SDL_LockMutex(p->mtx);
    p->seek_target = target;
    p->seek_req = 1;
    SDL_UnlockMutex(p->mtx);
}

double player_duration(Player *p) { return p->duration; }
bool   player_has_audio(Player *p) { return p->adev != 0; }

bool player_finished(Player *p) {
    SDL_LockMutex(p->mtx);
    bool fin = p->eof && !p->seek_req && p->qcount == 0 &&
               (!p->adev || SDL_GetQueuedAudioSize(p->adev) == 0);
    SDL_UnlockMutex(p->mtx);
    return fin;
}
