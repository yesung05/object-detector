#include "media.h"
#include "platform.h"

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 이 파일의 전체 흐름
 *
 * 압축 입력 파일
 *   → demux(컨테이너에서 비디오 패킷 꺼내기)
 *   → decode(압축 패킷을 픽셀 프레임으로 복원)
 *   → RGB24 변환
 *   → main.c의 콜백에서 감지하고 박스 그리기
 *   → 인코더용 색상 형식으로 변환
 *   → encode(프레임 압축)
 *   → mux(패킷을 출력 컨테이너에 기록)
 *
 * FFmpeg는 "보내기(send) / 받을 수 있을 만큼 받기(receive)" 방식이라 한 입력
 * 패킷에서 0개, 1개 또는 여러 프레임이 나올 수 있습니다.
 */

/* 출력 파일 하나를 만들 때 계속 유지하고 재사용하는 FFmpeg 자원입니다. */
typedef struct {
    AVFormatContext *format;
    AVCodecContext *encoder;
    AVStream *stream;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *sws;
    int64_t next_pts;
    int header_written;
} MediaWriter;

static double monotonic_seconds(void) {
    return platform_monotonic_seconds();
}

/* 호출자가 준 고정 크기 버퍼에 일반 C 오류 문자열을 기록합니다. */
static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || error_size == 0) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

/* FFmpeg의 음수 오류 코드를 사람이 읽을 수 있는 문자열로 변환합니다. */
static void set_av_error(char *error, size_t error_size, const char *operation,
                         int code) {
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, message, sizeof(message));
    set_error(error, error_size, "%s: %s", operation, message);
}

/* FFmpeg interrupt_callback의 고정 시그니처를 프로그램 콜백에 연결합니다. */
static int interrupt_ffmpeg(void *opaque) {
    const MediaOptions *options = (const MediaOptions *)opaque;
    if (!options || !options->interrupt_callback) return 0;
    return options->interrupt_callback(options->interrupt_opaque) != 0;
}

/* 프로그램 내부의 짧은 이름을 FFmpeg 코덱 ID로 바꿉니다. NONE이면 자동 선택입니다. */
static enum AVCodecID codec_from_name(const char *name) {
    if (!name || strcmp(name, "auto") == 0) return AV_CODEC_ID_NONE;
    if (strcmp(name, "mjpeg") == 0) return AV_CODEC_ID_MJPEG;
    if (strcmp(name, "mpeg4") == 0) return AV_CODEC_ID_MPEG4;
    if (strcmp(name, "h264") == 0) return AV_CODEC_ID_H264;
    if (strcmp(name, "h264_qsv") == 0) return AV_CODEC_ID_H264;
    return AV_CODEC_ID_NONE;
}

/*
 * FFmpeg 7 이전과 이후의 API 차이를 이 함수 안에 감춥니다.
 * 반환된 배열은 codec이 소유하므로 호출자가 free하면 안 됩니다.
 */
static const enum AVPixelFormat *supported_pixel_formats(const AVCodec *codec) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void *formats = NULL;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                     0, &formats, NULL) < 0)
        return NULL;
    return (const enum AVPixelFormat *)formats;
#else
    return codec->pix_fmts;
#endif
}

/* 인코더가 특정 픽셀 형식(RGB24, YUV420P 등)을 받는지 확인합니다. */
static int codec_supports(const AVCodec *codec, enum AVPixelFormat format) {
    const enum AVPixelFormat *pixel_format;
    const enum AVPixelFormat *formats = supported_pixel_formats(codec);
    if (!formats) return 1;
    for (pixel_format = formats;
         *pixel_format != AV_PIX_FMT_NONE; ++pixel_format) {
        if (*pixel_format == format) return 1;
    }
    return 0;
}

/*
 * 일반 영상 코덱은 RGB보다 YUV420P를 주로 사용합니다. YUV420P는 색상 정보를
 * 일부 줄여 RGB24보다 프레임 메모리가 작고 영상 압축에도 유리합니다.
 */
static enum AVPixelFormat choose_pixel_format(const AVCodec *codec) {
    const enum AVPixelFormat *formats;
    if (codec->id == AV_CODEC_ID_MJPEG &&
        codec_supports(codec, AV_PIX_FMT_YUVJ420P))
        return AV_PIX_FMT_YUVJ420P;
    if (codec_supports(codec, AV_PIX_FMT_YUV420P))
        return AV_PIX_FMT_YUV420P;
    if (codec_supports(codec, AV_PIX_FMT_RGB24))
        return AV_PIX_FMT_RGB24;
    formats = supported_pixel_formats(codec);
    return formats ? formats[0] : AV_PIX_FMT_YUV420P;
}

/*
 * 인코더 안에 준비된 압축 패킷을 모두 꺼내 출력 파일에 씁니다.
 * EAGAIN은 "지금은 더 받을 패킷이 없으니 다음 프레임을 보내라"는 정상 신호입니다.
 */
static int writer_drain(MediaWriter *writer, char *error, size_t error_size) {
    int ret;
    while (1) {
        ret = avcodec_receive_packet(writer->encoder, writer->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) {
            set_av_error(error, error_size, "avcodec_receive_packet", ret);
            return -1;
        }

        /*
         * 인코더와 출력 스트림은 시간 단위(time_base)가 다를 수 있으므로
         * 타임스탬프를 출력 스트림 단위로 환산합니다.
         */
        av_packet_rescale_ts(writer->packet, writer->encoder->time_base,
                             writer->stream->time_base);
        if (writer->packet->duration <= 0)
            writer->packet->duration =
                av_rescale_q(1, writer->encoder->time_base,
                             writer->stream->time_base);
        writer->packet->stream_index = writer->stream->index;
        ret = av_interleaved_write_frame(writer->format, writer->packet);

        /* AVPacket 구조체는 재사용하되 내부 데이터 참조는 매번 놓아줍니다. */
        av_packet_unref(writer->packet);
        if (ret < 0) {
            set_av_error(error, error_size, "av_interleaved_write_frame", ret);
            return -1;
        }
    }
}

/*
 * 첫 RGB 프레임의 해상도를 확인한 뒤 출력 컨테이너·인코더·색상 변환기를 엽니다.
 * 모든 큰 출력 버퍼는 여기서 한 번만 만들고 writer_close()까지 재사용합니다.
 */
static int writer_open(MediaWriter *writer, const char *path, int width,
                       int height, AVRational frame_rate,
                       const MediaOptions *options, char *error,
                       size_t error_size) {
    const AVCodec *codec;
    enum AVCodecID codec_id;
    AVDictionary *codec_options = NULL;
    AVDictionary *format_options = NULL;
    int ret;

    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = (AVRational){25, 1};
    }

    /* 파일 확장자(.jpg, .mp4 등)를 보고 출력 컨테이너 형식을 선택합니다. */
    ret = avformat_alloc_output_context2(&writer->format, NULL, NULL, path);
    if (ret < 0 || !writer->format) {
        set_av_error(error, error_size, "avformat_alloc_output_context2",
                     ret < 0 ? ret : AVERROR_UNKNOWN);
        return -1;
    }
    codec_id = codec_from_name(options->codec);
    if (codec_id == AV_CODEC_ID_NONE) codec_id = writer->format->oformat->video_codec;
    if (options->codec && strcmp(options->codec, "h264_qsv") == 0)
        codec = avcodec_find_encoder_by_name("h264_qsv");
    else
        codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        set_error(error, error_size,
                  "no encoder for codec '%s' and output '%s'",
                  options->codec ? options->codec : "auto", path);
        return -1;
    }
    writer->stream = avformat_new_stream(writer->format, NULL);
    writer->encoder = avcodec_alloc_context3(codec);
    writer->packet = av_packet_alloc();
    writer->frame = av_frame_alloc();
    if (!writer->stream || !writer->encoder || !writer->packet ||
        !writer->frame) {
        set_error(error, error_size, "out of memory creating video writer");
        return -1;
    }

    writer->encoder->width = width;
    writer->encoder->height = height;
    writer->encoder->pix_fmt = choose_pixel_format(codec);
    writer->encoder->time_base = av_inv_q(frame_rate);
    writer->encoder->framerate = frame_rate;

    /* 저사양 목표에 맞춰 인코더도 한 스레드만 사용합니다. */
    writer->encoder->thread_count = 1;
    writer->encoder->gop_size = 12;
    if (writer->format->oformat->flags & AVFMT_GLOBALHEADER)
        writer->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /* 코덱에 맞는 품질 설정을 적용합니다. 나머지는 단순 계산한 비트레이트를 씁니다. */
    if (codec_id == AV_CODEC_ID_MJPEG) {
        writer->encoder->flags |= AV_CODEC_FLAG_QSCALE;
        writer->encoder->global_quality = FF_QP2LAMBDA * options->quality;
    } else if (options->codec && strcmp(options->codec, "h264_qsv") == 0) {
        writer->encoder->bit_rate =
            (int64_t)width * (int64_t)height * frame_rate.num /
            frame_rate.den * 2;
    } else if (codec_id == AV_CODEC_ID_H264) {
        av_dict_set(&codec_options, "preset", "ultrafast", 0);
        {
            char quality[12];
            snprintf(quality, sizeof(quality), "%d", options->quality);
            av_dict_set(&codec_options, "crf", quality, 0);
        }
    } else {
        writer->encoder->bit_rate =
            (int64_t)width * (int64_t)height * frame_rate.num /
            frame_rate.den * 2;
    }

    ret = avcodec_open2(writer->encoder, codec, &codec_options);
    av_dict_free(&codec_options);
    if (ret < 0) {
        set_av_error(error, error_size, "avcodec_open2(encoder)", ret);
        return -1;
    }
    ret = avcodec_parameters_from_context(writer->stream->codecpar,
                                          writer->encoder);
    if (ret < 0) {
        set_av_error(error, error_size, "avcodec_parameters_from_context", ret);
        return -1;
    }
    writer->stream->time_base = writer->encoder->time_base;

    /*
     * 인코더에 보낼 AVFrame의 픽셀 저장 공간을 32바이트 정렬로 한 번 할당합니다.
     * 이 프레임은 writer_write()에서 계속 덮어쓰며 재사용합니다.
     */
    writer->frame->format = writer->encoder->pix_fmt;
    writer->frame->width = width;
    writer->frame->height = height;
    ret = av_frame_get_buffer(writer->frame, 32);
    if (ret < 0) {
        set_av_error(error, error_size, "av_frame_get_buffer", ret);
        return -1;
    }
    writer->sws = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                                 width, height, writer->encoder->pix_fmt,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!writer->sws) {
        set_error(error, error_size, "failed to create output color converter");
        return -1;
    }

    /* AVFMT_NOFILE이 아닌 일반 파일 컨테이너만 실제 파일 핸들을 엽니다. */
    if (!(writer->format->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&writer->format->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            set_av_error(error, error_size, "avio_open", ret);
            return -1;
        }
    }
    /* image2는 한 경로에 결과 이미지 한 장을 갱신하도록 설정합니다. */
    if (strcmp(writer->format->oformat->name, "image2") == 0)
        av_dict_set(&format_options, "update", "1", 0);
    ret = avformat_write_header(writer->format, &format_options);
    av_dict_free(&format_options);
    if (ret < 0) {
        set_av_error(error, error_size, "avformat_write_header", ret);
        return -1;
    }
    writer->header_written = 1;
    return 0;
}

/* 박스가 그려진 RGB 한 장을 색상 변환하고 인코더에 보냅니다. */
static int writer_write(MediaWriter *writer, const RgbFrame *rgb,
                        char *error, size_t error_size) {
    const uint8_t *source[4] = {rgb->data, NULL, NULL, NULL};
    int source_stride[4] = {rgb->stride, 0, 0, 0};

    /*
     * 인코더가 이전 프레임을 아직 참조 중이면 FFmpeg가 내부적으로 쓰기 가능한
     * 버퍼를 준비합니다. 호출자는 frame->data를 임의로 free하지 않습니다.
     */
    int ret = av_frame_make_writable(writer->frame);
    if (ret < 0) {
        set_av_error(error, error_size, "av_frame_make_writable", ret);
        return -1;
    }
    sws_scale(writer->sws, source, source_stride, 0, rgb->height,
              writer->frame->data, writer->frame->linesize);

    /* PTS는 프레임의 재생 시각입니다. CFR 출력이므로 매 프레임 1씩 증가합니다. */
    writer->frame->pts = writer->next_pts++;
    writer->frame->duration = 1;
    ret = avcodec_send_frame(writer->encoder, writer->frame);
    if (ret < 0) {
        set_av_error(error, error_size, "avcodec_send_frame", ret);
        return -1;
    }
    return writer_drain(writer, error, error_size);
}

/*
 * finish가 참이면 인코더에 NULL 프레임을 보내 내부에 남은 지연 프레임까지 꺼낸
 * 뒤 trailer를 기록합니다. 이후 성공/실패와 무관하게 모든 FFmpeg 자원을 해제합니다.
 */
static int writer_close(MediaWriter *writer, int finish,
                        char *error, size_t error_size) {
    int result = 0;
    if (!writer) return 0;
    if (finish && writer->encoder && writer->header_written) {
        /* NULL은 "입력이 끝났으니 내부 버퍼를 비우라(flush)"는 뜻입니다. */
        int ret = avcodec_send_frame(writer->encoder, NULL);
        if (ret >= 0 && writer_drain(writer, error, error_size) != 0)
            result = -1;
        if (result == 0) {
            writer->stream->duration =
                av_rescale_q(writer->next_pts, writer->encoder->time_base,
                             writer->stream->time_base);
            ret = av_write_trailer(writer->format);
            if (ret < 0) {
                set_av_error(error, error_size, "av_write_trailer", ret);
                result = -1;
            }
        }
    }
    sws_freeContext(writer->sws);
    av_frame_free(&writer->frame);
    av_packet_free(&writer->packet);
    avcodec_free_context(&writer->encoder);
    if (writer->format) {
        if (!(writer->format->oformat->flags & AVFMT_NOFILE))
            avio_closep(&writer->format->pb);
        avformat_free_context(writer->format);
    }

    /* 해제한 포인터가 남지 않도록 구조체 전체를 다시 0으로 만듭니다. */
    memset(writer, 0, sizeof(*writer));
    return result;
}

/*
 * 입력 한 개를 처리하는 동안 필요한 상태입니다.
 *
 * rgb_data는 디코더의 YUV 등 다양한 형식을 RGB24로 바꾼 한 장짜리 재사용 버퍼,
 * input_sws는 그 색상 변환 규칙, writer는 출력 인코딩 상태입니다.
 */
typedef struct {
    MediaWriter writer;
    struct SwsContext *input_sws;
    uint8_t *rgb_data[4];
    int rgb_stride[4];
    int width;
    int height;
    int64_t frame_index;
    int writer_opened;
    const char *output_path;
    const MediaOptions *options;
    AVRational frame_rate;
    FrameCallback callback;
    void *opaque;
} Pipeline;

/*
 * 디코더가 완성한 픽셀 프레임 한 장을 처리합니다.
 * 이 함수가 끝날 때까지 decoded는 FFmpeg 소유이고, rgb_data는 Pipeline 소유입니다.
 */
static int handle_decoded_frame(Pipeline *pipeline, const AVFrame *decoded,
                                char *error, size_t error_size) {
    RgbFrame rgb;
    int ret;
    double started;

    /* 첫 프레임에서만 RGB 버퍼를 할당합니다. 이후 프레임은 같은 버퍼에 덮어씁니다. */
    if (!pipeline->rgb_data[0]) {
        pipeline->width = decoded->width;
        pipeline->height = decoded->height;
        ret = av_image_alloc(pipeline->rgb_data, pipeline->rgb_stride,
                             decoded->width, decoded->height,
                             AV_PIX_FMT_RGB24, 32);
        if (ret < 0) {
            set_av_error(error, error_size, "av_image_alloc(RGB)", ret);
            return -1;
        }
    } else if (decoded->width != pipeline->width ||
               decoded->height != pipeline->height) {
        set_error(error, error_size,
                  "mid-stream resolution changes are not supported");
        return -1;
    }

    /*
     * sws_getCachedContext는 기존 변환기가 조건에 맞으면 재사용하고, 달라졌을 때만
     * 교체합니다. decoded의 YUV/RGB 형식이 무엇이든 RGB24 한 형식으로 통일합니다.
     */
    pipeline->input_sws = sws_getCachedContext(
        pipeline->input_sws, decoded->width, decoded->height,
        (enum AVPixelFormat)decoded->format, decoded->width, decoded->height,
        AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!pipeline->input_sws) {
        set_error(error, error_size, "failed to create input color converter");
        return -1;
    }
    started = monotonic_seconds();
    sws_scale(pipeline->input_sws,
              (const uint8_t *const *)decoded->data, decoded->linesize,
              0, decoded->height, pipeline->rgb_data, pipeline->rgb_stride);
    if (pipeline->options->stats)
        pipeline->options->stats->input_convert_seconds +=
            monotonic_seconds() - started;

    rgb.data = pipeline->rgb_data[0];
    rgb.width = pipeline->width;
    rgb.height = pipeline->height;
    rgb.stride = pipeline->rgb_stride[0];
    rgb.index = pipeline->frame_index++;

    /* main.c의 process_frame이 같은 rgb.data 위에 감지 박스와 태그를 그립니다. */
    started = monotonic_seconds();
    if (pipeline->callback(&rgb, pipeline->opaque, error, error_size) != 0)
        return -1;
    if (pipeline->options->stats)
        pipeline->options->stats->callback_seconds +=
            monotonic_seconds() - started;

    /* Preview-only camera mode intentionally skips conversion and encoding. */
    if (!pipeline->output_path) return 0;

    /* 출력 크기는 첫 디코딩 프레임에서 알 수 있으므로 writer도 이때 한 번 엽니다. */
    if (!pipeline->writer_opened) {
        if (writer_open(&pipeline->writer, pipeline->output_path, rgb.width,
                        rgb.height, pipeline->frame_rate, pipeline->options,
                        error, error_size) != 0)
            return -1;
        pipeline->writer_opened = 1;
    }
    started = monotonic_seconds();
    ret = writer_write(&pipeline->writer, &rgb, error, error_size);
    if (pipeline->options->stats)
        pipeline->options->stats->output_seconds += monotonic_seconds() - started;
    return ret;
}

/*
 * decoder가 현재 내놓을 수 있는 프레임을 모두 받습니다.
 * av_frame_unref는 AVFrame 구조체를 재사용할 수 있게 내부 버퍼 참조만 놓습니다.
 */
static int drain_decoder(AVCodecContext *decoder, AVFrame *frame,
                         Pipeline *pipeline, char *error, size_t error_size) {
    int ret;
    while (1) {
        ret = avcodec_receive_frame(decoder, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) {
            set_av_error(error, error_size, "avcodec_receive_frame", ret);
            return -1;
        }
        ret = handle_decoded_frame(pipeline, frame, error, error_size);
        av_frame_unref(frame);
        if (ret != 0) return -1;
        if (pipeline->options->max_frames > 0 &&
            pipeline->frame_index >= pipeline->options->max_frames)
            return 1;
    }
}

int media_process(const char *input_path, const char *output_path,
                  const MediaOptions *options, FrameCallback callback,
                  void *opaque, char *error, size_t error_size) {
    AVFormatContext *input = NULL;
    const AVInputFormat *input_format = NULL;
    AVCodecContext *decoder = NULL;
    const AVCodec *codec;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    AVDictionary *input_options = NULL;
    Pipeline pipeline;
    int video_stream;
    int ret;
    int result = -1;
    int finish_output = 0;

    /* Pipeline의 모든 포인터와 카운터를 안전한 0/NULL 상태로 시작합니다. */
    memset(&pipeline, 0, sizeof(pipeline));
    av_log_set_level(AV_LOG_ERROR);
    if (!input_path || !options || !callback ||
        (output_path && strcmp(input_path, output_path) == 0)) {
        set_error(error, error_size,
                  "input must be present and differ from an optional output");
        return -1;
    }
    pipeline.output_path = output_path;
    pipeline.options = options;
    pipeline.callback = callback;
    pipeline.opaque = opaque;
    if (options->stats) memset(options->stats, 0, sizeof(*options->stats));

    /*
     * input_format이 있으면 파일 자동 감지가 아니라 libavdevice의 카메라 입력을
     * 명시적으로 엽니다. avfoundation과 v4l2 모두 같은 avformat 디코딩 흐름을
     * 사용하므로 이후 추론 코드는 파일 입력과 완전히 동일합니다.
     */
    if (options->input_format) {
        avdevice_register_all();
        input_format = av_find_input_format(options->input_format);
        if (!input_format) {
            set_error(error, error_size,
                      "camera input format '%s' is unavailable in this FFmpeg build",
                      options->input_format);
            goto done;
        }
        if (options->video_size)
            av_dict_set(&input_options, "video_size", options->video_size, 0);
        if (options->framerate)
            av_dict_set(&input_options, "framerate", options->framerate, 0);
        if (strcmp(options->input_format, "avfoundation") == 0) {
            /* Most current Mac cameras expose NV12 but not planar yuv420p. */
            av_dict_set(&input_options, "pixel_format", "nv12", 0);
            av_dict_set(&input_options, "drop_late_frames", "1", 0);
        }
    }

    /* 카메라 읽기가 블로킹 중이어도 Ctrl+C 요청을 감지할 수 있게 미리 설정합니다. */
    input = avformat_alloc_context();
    if (!input) {
        set_error(error, error_size, "out of memory creating input context");
        goto done;
    }
    input->interrupt_callback.callback = interrupt_ffmpeg;
    input->interrupt_callback.opaque = (void *)options;
    if (options->realtime) {
        input->flags |= AVFMT_FLAG_NOBUFFER;
        input->max_delay = 0;
    }

    /*
     * AVFormatContext는 MP4/JPEG 같은 컨테이너를 읽고, 그 안의 스트림 정보를
     * 제공합니다. 여기서는 가장 적합한 비디오 스트림 하나만 선택하고 오디오는
     * 의도적으로 무시합니다.
     */
    ret = avformat_open_input(&input, input_path, input_format, &input_options);
    if (ret < 0) {
        if (interrupt_ffmpeg((void *)options)) {
            result = 0;
            goto done;
        }
        set_av_error(error, error_size, "avformat_open_input", ret);
        goto done;
    }
    ret = avformat_find_stream_info(input, NULL);
    if (ret < 0) {
        if (interrupt_ffmpeg((void *)options)) {
            result = 0;
            goto done;
        }
        set_av_error(error, error_size, "avformat_find_stream_info", ret);
        goto done;
    }
    video_stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       &codec, 0);
    if (video_stream < 0) {
        set_av_error(error, error_size, "av_find_best_stream", video_stream);
        goto done;
    }
    decoder = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!decoder || !frame || !packet) {
        set_error(error, error_size, "out of memory creating video reader");
        goto done;
    }
    ret = avcodec_parameters_to_context(
        decoder, input->streams[video_stream]->codecpar);
    if (ret < 0) {
        set_av_error(error, error_size, "avcodec_parameters_to_context", ret);
        goto done;
    }
    /* 디코더 역시 한 스레드로 제한해 자원 사용량을 예측하기 쉽게 합니다. */
    decoder->thread_count = 1;
    if (options->realtime) decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ret = avcodec_open2(decoder, codec, NULL);
    if (ret < 0) {
        set_av_error(error, error_size, "avcodec_open2(decoder)", ret);
        goto done;
    }
    pipeline.frame_rate =
        av_guess_frame_rate(input, input->streams[video_stream], NULL);
    if (options->stats && pipeline.frame_rate.num > 0 &&
        pipeline.frame_rate.den > 0)
        options->stats->frame_rate = av_q2d(pipeline.frame_rate);

    /*
     * av_read_frame이 주는 AVPacket은 아직 압축된 데이터입니다.
     * 비디오 패킷만 decoder에 보내고, 오디오/자막 패킷은 즉시 unref합니다.
     */
    while (!interrupt_ffmpeg((void *)options)) {
        ret = av_read_frame(input, packet);
        if (ret == AVERROR(EAGAIN)) {
            /* Live capture may temporarily have no frame; this is not EOF. */
            platform_sleep_milliseconds(1);
            continue;
        }
        if (ret < 0) break;
        if (packet->stream_index == video_stream) {
            ret = avcodec_send_packet(decoder, packet);

            /* decoder가 패킷 데이터를 참조한 뒤에는 이 Packet을 다음 읽기에 재사용합니다. */
            av_packet_unref(packet);
            if (ret < 0) {
                set_av_error(error, error_size, "avcodec_send_packet", ret);
                goto done;
            }
            ret = drain_decoder(decoder, frame, &pipeline, error, error_size);
            if (ret < 0) goto done;
            if (ret > 0) {
                finish_output = 1;
                result = 0;
                goto done;
            }
        } else {
            av_packet_unref(packet);
        }
    }
    if (interrupt_ffmpeg((void *)options)) {
        finish_output = pipeline.writer_opened;
        result = 0;
        goto done;
    }
    if (ret != AVERROR_EOF) {
        set_av_error(error, error_size, "av_read_frame", ret);
        goto done;
    }
    /* 입력 끝에서 NULL 패킷을 보내 디코더 내부에 남은 프레임까지 모두 받습니다. */
    ret = avcodec_send_packet(decoder, NULL);
    if (ret < 0) {
        set_av_error(error, error_size, "flush decoder", ret);
        goto done;
    }
    ret = drain_decoder(decoder, frame, &pipeline, error, error_size);
    if (ret < 0) goto done;
    if (pipeline.frame_index == 0) {
        set_error(error, error_size, "input contains no decoded video frames");
        goto done;
    }
    finish_output = 1;
    result = 0;

done:
    /*
     * 하나의 정리 지점에서 생성 순서의 반대로 모두 해제합니다.
     * FFmpeg의 *_free 함수는 대부분 NULL 포인터도 안전하게 처리합니다.
     */
    if (writer_close(&pipeline.writer, finish_output, error, error_size) != 0)
        result = -1;
    sws_freeContext(pipeline.input_sws);
    av_freep(&pipeline.rgb_data[0]);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    av_dict_free(&input_options);
    if (options && options->stats)
        options->stats->frames = pipeline.frame_index;
    return result;
}
