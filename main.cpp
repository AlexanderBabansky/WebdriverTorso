#include <iostream>
#include <chrono>
#include <thread>
#include <cassert>
#include <list>
#include <mutex>
#include <signal.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
}

using namespace std;

AVCodecContext* c_video = NULL;
AVCodecContext* c_audio = NULL;
AVPacket* pkt_video = NULL;
AVPacket* pkt_audio = NULL;
AVFrame* frame_video = NULL;
AVFrame* frame_audio = NULL;
AVStream* stream_video = NULL;
AVStream* stream_audio = NULL;
AVFormatContext* oc = NULL;
std::mutex packets_mutex;
std::list<AVPacket*> packets_video;
std::list<AVPacket*> packets_audio;
std::condition_variable packets_cv;

bool verbose = true;
std::atomic_bool stop_flag = false;

void sigintHandler(int sig_num)
{
    signal(SIGINT, sigintHandler);
    printf("\n Terminating with Ctrl+C \n");
    stop_flag = true;
}


int init_video_codec(AVCodecContext* c, const AVCodec* codec) {
    /* put sample parameters */
    c->bit_rate = 2500000;
    /* resolution must be a multiple of two */
    c->width = 1920;
    c->height = 1080;
    /* frames per second */
    c->time_base = { 1, 25 };
    c->framerate = { 25, 1 };

    c->gop_size = 10;
    c->max_b_frames = 1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (codec->id == AV_CODEC_ID_H264)
        av_opt_set(c->priv_data, "preset", "veryfast", 0);

    int ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        char errbuf[100]{ 0 };
        av_make_error_string(errbuf, 100, ret);
        fprintf(stderr, "Could not open codec: %s\n", errbuf);
        exit(1);
    }

    return 0;
}

int init_audio_codec(AVCodecContext* c, const AVCodec* codec) {
    /* put sample parameters */
    c->bit_rate = 320000;
    /* check that the encoder supports s16 pcm input */
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->sample_rate = 48000;
    av_channel_layout_default(&c->ch_layout, 2);
    c->channels = 2;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }
    return 0;
}

void free_codecs() {
    avcodec_free_context(&c_video);
    avcodec_free_context(&c_audio);
}

int init_codecs() {
    const AVCodec* codec_video = avcodec_find_encoder(AV_CODEC_ID_H264);
    //codec_video = avcodec_find_encoder_by_name("libopenh264");
    if (!codec_video) {
        fprintf(stderr, "Codec '%s' not found\n", "aac");
        exit(1);
    }
    const AVCodec* codec_audio = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec_audio) {
        fprintf(stderr, "Codec '%s' not found\n", "aac");
        exit(1);
    }

    c_video = avcodec_alloc_context3(codec_video);
    if (!c_video) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    c_audio = avcodec_alloc_context3(codec_audio);
    if (!c_audio) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

    init_video_codec(c_video, codec_video);
    init_audio_codec(c_audio, codec_audio);
    return 0;
}

uint64_t t = 0;

int generate_audio_frame(AVFrame* frame, float freq) {
    int ret = av_frame_make_writable(frame);
    if (ret < 0)
        exit(1);
    float tincr = 2 * M_PI * freq / frame->sample_rate;

    for (int j = 0; j < frame->nb_samples; j++) {
        float v = (sin(t * tincr));

        for (int k = 0; k < frame->ch_layout.nb_channels; k++) {
            float* samples = (float*)frame->data[k];
            samples[j] = v;
        }
        t++;
    }
    return 0;
}

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

Rect blueRect;
Rect redRect;

Rect generate_rect(int width, int height) {
    assert(width >= height);
    int minWidth = height / 10;
    int maxWidth = height / 2;

    int w = rand() % (maxWidth - minWidth) + minWidth;
    int h = rand() % (maxWidth - minWidth) + minWidth;
    w -= w % 2;
    h -= h % 2;

    int x = rand() % (width - w);
    int y = rand() % (height - h);
    x -= x % 2;
    y -= y % 2;

    Rect res;
    res.width = w;
    res.height = h;
    res.x = x;
    res.y = y;
    return res;
}

int change_rects(int width, int height) {
    blueRect = generate_rect(width, height);
    redRect = generate_rect(width, height);
    return 0;
}

struct YUVColor {
    int Y = 0;
    int U = 0;
    int V = 0;
};

YUVColor get_yuv_from_rgb(int R, int G, int B) {
    int Y = 0.183f * R + 0.614f * G + 0.062f * B + 16;
    int U = -0.101f * R - 0.338f * G + 0.439f * B + 128;
    int V = 0.439f * R - 0.399f * G - 0.040f * B + 128;
    YUVColor c;
    c.Y = Y;
    c.U = U;
    c.V = V;
    return c;
}


void clean_frame(AVFrame* frame) {
    memset(frame->data[0], 255, frame->linesize[0] * frame->height);
    memset(frame->data[1], 128, frame->linesize[1] * frame->height / 2);
    memset(frame->data[2], 128, frame->linesize[2] * frame->height / 2);
}

void draw_rect_on_frame(AVFrame* frame, Rect rect, YUVColor color) {
    for (int y = rect.y; y < rect.y + rect.height; y++, y++) {
        memset(frame->data[0] + frame->linesize[0] * (y)+rect.x, color.Y, rect.width);
        memset(frame->data[0] + frame->linesize[0] * (y + 1) + rect.x, color.Y, rect.width);
        memset(frame->data[1] + (frame->linesize[1] * (y / 2)) + rect.x / 2, color.U, rect.width / 2);
        memset(frame->data[2] + frame->linesize[2] * (y / 2) + rect.x / 2, color.V, rect.width / 2);
    }
}

int generate_video_frame(AVFrame* frame, Rect blueRect) {
    int ret = av_frame_make_writable(frame);
    if (ret < 0)
        exit(1);
    clean_frame(frame);
    draw_rect_on_frame(frame, blueRect, get_yuv_from_rgb(0, 0, 255));
    draw_rect_on_frame(frame, redRect, get_yuv_from_rgb(255, 0, 0));
    return 0;
}

int output_video(AVPacket* pkt) {
    av_packet_rescale_ts(pkt, { c_video->time_base.num, c_video->time_base.den }, { 1,1000 });
    AVPacket* pkt_copy = av_packet_clone(pkt);
    std::unique_lock<std::mutex> g(packets_mutex);
    packets_video.push_back(pkt_copy);
    packets_cv.notify_one();
    return 0;
}

int output_audio(AVPacket* pkt) {
    av_packet_rescale_ts(pkt, { c_audio->time_base.num, c_audio->time_base.den }, { 1,1000 });
    AVPacket* pkt_copy = av_packet_clone(pkt);
    std::unique_lock<std::mutex> g(packets_mutex);
    packets_audio.push_back(pkt_copy);
    packets_cv.notify_one();
    return 0;
}

int encode(AVFrame* frame, AVCodecContext* c, AVPacket* pkt, int (*output_video)(AVPacket*)) {
    int ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            exit(1);
        }
        output_video(pkt);
        av_packet_unref(pkt);
    }
    return 0;
}

std::string getLibavError(int c) {
    char errbuf[200];
    av_make_error_string(errbuf, 200, c);
    return errbuf;
}

int network_thread(std::atomic_bool* stop_flag, std::atomic_bool* err_flag) {
    while (*stop_flag == false) {
        std::unique_lock<std::mutex> g(packets_mutex);
        while (*stop_flag == false && (packets_audio.empty() || packets_video.empty())) {
            packets_cv.wait(g);
            continue;
        }
        if (*stop_flag)
            break;

        AVPacket* pkt_audio = packets_audio.front();
        AVPacket* pkt_video = packets_video.front();

        pkt_video->stream_index = stream_video->id;
        pkt_audio->stream_index = stream_audio->id;

        if (pkt_audio->dts < pkt_video->dts) {
            if (verbose) {
                printf("Send audio packet. dts: %d\n", pkt_audio->dts);
            }
            int ret = av_interleaved_write_frame(oc, pkt_audio);
            if (ret < 0) {
                fprintf(stderr, "Error sending audio packet\n");
                *err_flag = true;
                return 1;
            }
            packets_audio.pop_front();
            av_packet_free(&pkt_audio);
        }
        else {
            if (verbose) {
                printf("Send video packet. dts: %d\n", pkt_video->dts);
            }
            int ret = av_interleaved_write_frame(oc, pkt_video);
            if (ret < 0) {
                fprintf(stderr, "Error sending video packet\n");
                *err_flag = true;
                return 1;
            }
            packets_video.pop_front();
            av_packet_free(&pkt_video);
        }
    }
    return 0;
}

int64_t get_avframe_pts_ms(AVFrame* frame, AVRational time_base) {
    return 1000 * frame->pts * time_base.num / time_base.den;
}

int main(int argc, char** argv) {
    signal(SIGINT, sigintHandler);
    srand(time(NULL));

    if (argc != 2) {
        printf("Need url\n");
        return 1;
    }
    const char* url = argv[1];
    init_codecs();
    if (verbose) {
        printf("Inited encoders\n");
    }

    pkt_video = av_packet_alloc();
    pkt_audio = av_packet_alloc();
    if (!pkt_video || !pkt_audio)
        return 1;

    frame_video = av_frame_alloc();
    frame_audio = av_frame_alloc();
    if (!frame_video || !frame_audio)
        return 1;

    frame_video->format = c_video->pix_fmt;
    frame_video->width = c_video->width;
    frame_video->height = c_video->height;
    frame_video->pts = 0;
    int ret = av_frame_get_buffer(frame_video, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        return 1;
    }

    frame_audio->pts = 0;
    frame_audio->nb_samples = c_audio->frame_size;
    frame_audio->format = c_audio->sample_fmt;
    frame_audio->sample_rate = c_audio->sample_rate;
    ret = av_channel_layout_copy(&frame_audio->ch_layout, &c_audio->ch_layout);
    if (ret < 0)
        exit(1);

    /* allocate the data buffers */
    ret = av_frame_get_buffer(frame_audio, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate audio data buffers\n");
        exit(1);
    }

    if (avformat_alloc_output_context2(&oc, NULL, "flv", url) < 0) {
        fprintf(stderr, "Could not allocate output context\n");
        return 1;
    }
    stream_video = avformat_new_stream(oc, NULL);
    if (!stream_video) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    stream_video->id = oc->nb_streams - 1;

    stream_audio = avformat_new_stream(oc, NULL);
    if (!stream_audio) {
        fprintf(stderr, "Could not allocate stream\n");
        return 1;
    }
    stream_audio->id = oc->nb_streams - 1;

    if (avcodec_parameters_from_context(stream_video->codecpar, c_video) < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        return 1;
    }
    if (avcodec_parameters_from_context(stream_audio->codecpar, c_audio) < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        return 1;
    }

    if (verbose) {
        printf("Try to connect...\n");
    }
    ret = avio_open(&oc->pb, url, AVIO_FLAG_WRITE);
    if (ret < 0) {
        fprintf(stderr, "Could not open filename '%s': %s\n", url,
            getLibavError(ret).c_str());
        return 1;
    }
    if (verbose) {
        printf("Connected\n");
    }

    AVDictionary* opt = NULL;
    ret = avformat_write_header(oc, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
            getLibavError(ret).c_str());
        return 1;
    }

    std::atomic_bool err_flag = false;
    std::thread th(network_thread, &stop_flag, &err_flag);


    change_rects(c_video->width, c_video->height);
    float freq = 440;

    int change_interval = 25;
    bool changed_frame = false;
    int64_t video_pts_ms = -1000;
    int64_t audio_pts_ms = -1000;
    chrono::high_resolution_clock::time_point start_time = chrono::high_resolution_clock::now();

    for (;;) {
        if (stop_flag) {
            break;
        }
        if (err_flag) {
            fprintf(stderr, "Error occurred in network thread\n");
            break;
        }
        if (frame_video->pts % change_interval == 0 && !changed_frame) {
            change_rects(c_video->width, c_video->height);
            generate_video_frame(frame_video, blueRect);
            freq = rand() % 400 + 200;
            changed_frame = true;
        }
        if (video_pts_ms < audio_pts_ms) {
            changed_frame = false;
            encode(frame_video, c_video, pkt_video, &output_video);
            frame_video->pts++;
            video_pts_ms = get_avframe_pts_ms(frame_video, c_video->time_base);
        }
        else {
            generate_audio_frame(frame_audio, freq);
            encode(frame_audio, c_audio, pkt_audio, &output_audio);
            frame_audio->pts += c_audio->frame_size;
            audio_pts_ms = get_avframe_pts_ms(frame_audio, c_audio->time_base);
        }

        int64_t elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now() - start_time).count();
        if (video_pts_ms > elapsed_ms) {
            this_thread::sleep_for(chrono::milliseconds(video_pts_ms - elapsed_ms));
        }
    }

    if (verbose) {
        printf("Terminating...\n");
    }

    stop_flag = true;
    {
        std::unique_lock<std::mutex> g(packets_mutex);
        packets_cv.notify_one();
    }
    th.join();
    free_codecs();
    av_packet_free(&pkt_video);
    av_packet_free(&pkt_audio);
    av_frame_free(&frame_video);
    av_frame_free(&frame_audio);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    for (auto& p : packets_audio) {
        av_packet_free(&p);
    }
    for (auto& p : packets_video) {
        av_packet_free(&p);
    }
    packets_audio.clear();
    packets_video.clear();
    return 0;
}
