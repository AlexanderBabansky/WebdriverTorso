#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <stdio.h>

#include <iostream>
#include <chrono>
#include <thread>
#include "easyrtmp/data_layers/tcp_network.h"
#include "easyrtmp/rtmp_client_session.h"
#include "easyrtmp/utils.h"
#include "easyrtmp/rtmp_exception.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include <libavutil/opt.h>
#include <libavutil/mem.h>

#include <libavformat/avio.h>
}

extern "C" {
    typedef struct NALU {
        int offset;
        uint32_t size;
    } NALU;

    typedef struct NALUList {
        NALU* nalus;
        unsigned nalus_array_size;
        unsigned nb_nalus;          ///< valid entries in nalus
    } NALUList;


    static const uint8_t* avc_find_startcode_internal(const uint8_t* p, const uint8_t* end)
    {
        const uint8_t* a = p + 4 - ((intptr_t)p & 3);

        for (end -= 3; p < a && p < end; p++) {
            if (p[0] == 0 && p[1] == 0 && p[2] == 1)
                return p;
        }

        for (end -= 3; p < end; p += 4) {
            uint32_t x = *(const uint32_t*)p;
            //      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
            //      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
            if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
                if (p[1] == 0) {
                    if (p[0] == 0 && p[2] == 1)
                        return p;
                    if (p[2] == 0 && p[3] == 1)
                        return p + 1;
                }
                if (p[3] == 0) {
                    if (p[2] == 0 && p[4] == 1)
                        return p + 2;
                    if (p[4] == 0 && p[5] == 1)
                        return p + 3;
                }
            }
        }

        for (end += 3; p < end; p++) {
            if (p[0] == 0 && p[1] == 0 && p[2] == 1)
                return p;
        }

        return end + 3;
    }


    const uint8_t* ff_avc_find_startcode(const uint8_t* p, const uint8_t* end) {
        const uint8_t* out = avc_find_startcode_internal(p, end);
        if (p < out && out < end && !out[-1]) out--;
        return out;
    }

    static int avc_parse_nal_units(AVIOContext* pb, NALUList* list,
        const uint8_t* buf_in, int size)
    {
        const uint8_t* p = buf_in;
        const uint8_t* end = p + size;
        const uint8_t* nal_start, * nal_end;

        size = 0;
        nal_start = ff_avc_find_startcode(p, end);
        for (;;) {
            const size_t nalu_limit = SIZE_MAX / sizeof(*list->nalus);
            while (nal_start < end && !*(nal_start++));
            if (nal_start == end)
                break;

            nal_end = ff_avc_find_startcode(nal_start, end);
            if (pb) {
                avio_wb32(pb, nal_end - nal_start);
                avio_write(pb, nal_start, nal_end - nal_start);
            }
            else if (list->nb_nalus >= nalu_limit) {
                return AVERROR(ERANGE);
            }
            else {

                NALU* tmp = (NALU*)av_fast_realloc(list->nalus, &list->nalus_array_size,
                    (list->nb_nalus + 1) * sizeof(*list->nalus));
                if (!tmp)
                    return AVERROR(ENOMEM);
                list->nalus = tmp;
                tmp[list->nb_nalus++] = {};
                tmp[list->nb_nalus++].offset = nal_start - p;
                tmp[list->nb_nalus++].size = nal_end - nal_start;
            }
            size += 4 + nal_end - nal_start;
            nal_start = nal_end;
        }
        return size;
    }

    int ff_avc_parse_nal_units(AVIOContext* pb, const uint8_t* buf_in, int size)
    {
        return avc_parse_nal_units(pb, NULL, buf_in, size);
    }

    int ff_avc_parse_nal_units_buf(const uint8_t* buf_in, uint8_t** buf, int* size)
    {
        AVIOContext* pb;
        int ret = avio_open_dyn_buf(&pb);
        if (ret < 0)
            return ret;

        ff_avc_parse_nal_units(pb, buf_in, *size);

        *size = avio_close_dyn_buf(pb, buf);
        return 0;
    }
}

using namespace std;

AVCodecContext* c_video = NULL;
AVCodecContext* c_audio = NULL;
AVPacket* pkt_video = NULL;
AVPacket* pkt_audio = NULL;
AVFrame* frame_video = NULL;
AVFrame* frame_audio = NULL;

int init_video_codec(AVCodecContext* c, const AVCodec* codec) {
    /* put sample parameters */
    c->bit_rate = 2500000;
    /* resolution must be a multiple of two */
    c->width = 1920;
    c->height = 1080;
    /* frames per second */
    c->time_base = { 1, 25 };
    c->framerate = { 25, 1 };

    //c->gop_size = 10;
    //c->max_b_frames = 1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    //if (codec->id == AV_CODEC_ID_H264)
        //av_opt_set(c->priv_data, "preset", "veryfast", 0);

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

int init_codecs() {
    const AVCodec* codec_video = avcodec_find_encoder(AV_CODEC_ID_H264);
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

int generate_audio_frame(AVFrame* frame, AVCodecContext* c) {
    int ret = av_frame_make_writable(frame);
    if (ret < 0)
        exit(1);
    float tincr = 2 * M_PI * 440.0 / c->sample_rate;

    for (int j = 0; j < c->frame_size; j++) {
        float v = (sin(t * tincr));

        for (int k = 0; k < c->ch_layout.nb_channels; k++) {
            float* samples = (float*)frame->data[k];
            samples[j] = v;
        }

        t++;
    }
    return 0;
}

int generate_video_frame(AVFrame* frame, AVCodecContext* c, int i) {
    int ret = av_frame_make_writable(frame);
    if (ret < 0)
        exit(1);

    /* Y */
    for (int y = 0; y < c->height; y++) {
        for (int x = 0; x < c->width; x++) {
            frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
        }
    }

    /* Cb and Cr */
    for (int y = 0; y < c->height / 2; y++) {
        for (int x = 0; x < c->width / 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
            frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
        }
    }
    return 0;
}

int64_t video_pts_ms = 0;
int64_t audio_pts_ms = 0;

int output_video(AVPacket* pkt, librtmp::RTMPClientSession& rtmp) {
    av_packet_rescale_ts(pkt, { c_video->time_base.num, c_video->time_base.den }, { 1,1000 });
    if (pkt->dts < 0)
        pkt->dts = 0;
    librtmp::RTMPMediaMessage mediaMsg;
    mediaMsg.message_type = librtmp::RTMPMessageType::VIDEO;
    mediaMsg.timestamp = pkt->dts;
    mediaMsg.video.d.avc_packet_type = 1;
    mediaMsg.video.d.composition_time = pkt->pts - pkt->dts;
    mediaMsg.video.d.codec_id = (int)(librtmp::RTMPVideoCodec::AVC);
    mediaMsg.video.d.frame_type = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 2;
    mediaMsg.video.video_data_send.resize(pkt->size);
    memcpy(mediaMsg.video.video_data_send.data(), pkt->data, pkt->size);
    rtmp.SendRTMPMessage(mediaMsg);
    std::cout << "Out Video " << pkt->dts << endl;
    video_pts_ms = pkt->dts;
    return 0;
}

int output_audio(AVPacket* pkt, librtmp::RTMPClientSession& rtmp) {
    av_packet_rescale_ts(pkt, { c_audio->time_base.num, c_audio->time_base.den }, { 1,1000 });
    if (pkt->dts < 0)
        pkt->dts = 0;

    librtmp::RTMPMediaMessage mediaMsg;
    mediaMsg.message_type = librtmp::RTMPMessageType::AUDIO;
    mediaMsg.timestamp = pkt->dts;
    mediaMsg.audio.aac_packet_type = 1;
    mediaMsg.audio.d.channels = 1;
    mediaMsg.audio.d.format = (int)librtmp::RTMPAudioCodec::AAC;
    mediaMsg.audio.d.sample_rate = 3;
    mediaMsg.audio.d.sample_size = 1;
    mediaMsg.audio.audio_data_send.resize(pkt->size);
    memcpy(mediaMsg.audio.audio_data_send.data(), pkt->data, pkt->size);
    rtmp.SendRTMPMessage(mediaMsg);
    std::cout << "Out Audio " << pkt->dts << endl;
    audio_pts_ms = pkt->dts;
    return 0;
}

int encode(AVFrame* frame, AVCodecContext* c, AVPacket* pkt, librtmp::RTMPClientSession& rtmp, int (*output_video)(AVPacket*, librtmp::RTMPClientSession&)) {
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
        uint8_t* data = NULL;
        int size = pkt->size;

        //ret = ff_avc_parse_nal_units_buf(pkt->data, &data, &size);
        //av_free(data);
        output_video(pkt, rtmp);
        av_packet_unref(pkt);
    }
    return 0;
}

WSADATA wsaData;

void init_network() {
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        exit(1);
    }
}
int main() {
    std::cout << "Hello" << endl;
    init_network();

    init_codecs();

    pkt_video = av_packet_alloc();
    pkt_audio = av_packet_alloc();
    if (!pkt_video)
        exit(1);
    if (!pkt_audio)
        exit(1);

    frame_video = av_frame_alloc();
    frame_audio = av_frame_alloc();
    if (!frame_video) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    if (!frame_audio) {
        fprintf(stderr, "Could not allocate audio frame\n");
        exit(1);
    }

    frame_video->format = c_video->pix_fmt;
    frame_video->width = c_video->width;
    frame_video->height = c_video->height;
    int ret = av_frame_get_buffer(frame_video, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        exit(1);
    }


    frame_audio->nb_samples = c_audio->frame_size;
    frame_audio->format = c_audio->sample_fmt;
    ret = av_channel_layout_copy(&frame_audio->ch_layout, &c_audio->ch_layout);
    if (ret < 0)
        exit(1);

    /* allocate the data buffers */
    ret = av_frame_get_buffer(frame_audio, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate audio data buffers\n");
        exit(1);
    }


    librtmp::ParsedUrl parsed_url;
    parsed_url.app = "live2";
    parsed_url.key = "j0c0-v7xv-4kmb-gtu7-5r5p";
    parsed_url.port = 1935;
    parsed_url.type = librtmp::ProtoType::RTMP;
    parsed_url.url = "a.rtmp.youtube.com";

    TCPClient tcp_client;
    std::shared_ptr<TCPNetwork> tcp_network;

    try {
        tcp_network = tcp_client.ConnectToHost(parsed_url.url.c_str(), parsed_url.port);
        librtmp::RTMPEndpoint rtmp_endpoint(tcp_network.get());
        librtmp::RTMPClientSession rtmp_client(&rtmp_endpoint);

        librtmp::ClientParameters client_parameters;
        client_parameters.app = parsed_url.app;
        client_parameters.url = parsed_url.url;
        client_parameters.key = parsed_url.key;
        client_parameters.has_video = true;
        client_parameters.width = c_video->width;
        client_parameters.height = c_video->height;
        client_parameters.video_datarate = c_video->bit_rate / 1024;
        client_parameters.framerate = c_video->framerate.num / c_video->framerate.den;
        client_parameters.video_codec = librtmp::RTMPVideoCodec::AVC;
        client_parameters.has_audio = true;
        client_parameters.audio_codec = librtmp::RTMPAudioCodec::AAC;
        client_parameters.audio_datarate = c_audio->bit_rate / 1024;
        client_parameters.channels = c_audio->ch_layout.nb_channels;
        client_parameters.samplesize = 32;
        client_parameters.samplerate = c_audio->sample_rate;
        rtmp_client.SendClientParameters(&client_parameters);

        {
            librtmp::RTMPMediaMessage mediaMsg;
            mediaMsg.message_type = librtmp::RTMPMessageType::VIDEO;
            mediaMsg.timestamp = 0;
            mediaMsg.video.d.avc_packet_type = 0;
            mediaMsg.video.d.codec_id = (int)(librtmp::RTMPVideoCodec::AVC);
            mediaMsg.video.d.frame_type = 1;
            mediaMsg.video.video_data_send.resize(c_video->extradata_size);
            memcpy(mediaMsg.video.video_data_send.data(), c_video->extradata,
                c_video->extradata_size);
            rtmp_client.SendRTMPMessage(mediaMsg);
        }

        {
            librtmp::RTMPMediaMessage mediaMsg;
            mediaMsg.message_type = librtmp::RTMPMessageType::AUDIO;
            mediaMsg.timestamp = 0;
            mediaMsg.audio.aac_packet_type = 0;
            mediaMsg.audio.d.channels = 1; //0 - mono, 1 - stereo
            mediaMsg.audio.d.format = (int)librtmp::RTMPAudioCodec::AAC;
            mediaMsg.audio.d.sample_rate
                = 3; //0 - 5.5 Khz, 1 - 11 Khz, 2 - 22 Khz, 3 - 44 KHz
            mediaMsg.audio.d.sample_size = 1; //0 - 8 bit, 1 - 16 bit
            mediaMsg.audio.audio_data_send.resize(c_audio->extradata_size);
            memcpy(mediaMsg.audio.audio_data_send.data(),
                c_audio->extradata, c_audio->extradata_size);
            rtmp_client.SendRTMPMessage(mediaMsg);
        }
        
        chrono::high_resolution_clock::time_point start_time = chrono::high_resolution_clock::now();
        int64_t video_pts = 0;
        int64_t audio_pts = 0;
        for (int i = 0;; i++) {            
            if (video_pts % 25 == 0) {
                //std::cout << "Send video frames " << i << endl;
            }
            if (video_pts_ms < audio_pts_ms) {
                generate_video_frame(frame_video, c_video, i);
                frame_video->pts = video_pts;
                video_pts++;
                encode(frame_video, c_video, pkt_video, rtmp_client, &output_video);
            }
            else {
                generate_audio_frame(frame_audio, c_audio);
                frame_audio->pts = audio_pts;
                audio_pts += c_audio->frame_size;
                encode(frame_audio, c_audio, pkt_audio, rtmp_client, &output_audio);
            }

            int64_t elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now() - start_time).count();
            if (video_pts_ms > elapsed_ms) {
                this_thread::sleep_for(chrono::milliseconds(video_pts_ms - elapsed_ms));
            }
        }

    }
    catch (TCPNetworkException& e) {
        std::cout << "Connection error: " << e.what() << endl;
        return 1;
    }
    catch (std::exception& e) {
        std::cout << "Unexpected error: " << e.what() << endl;
        return 1;
    }

    return 0;
}
