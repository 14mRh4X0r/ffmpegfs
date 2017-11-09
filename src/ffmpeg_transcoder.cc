/*
 * FFMPEG decoder class source for mp3fs
 *
 * Copyright (C) 2015 Thomas Schwarzenberger
 * FFMPEG supplementals by Norbert Schlia (nschlia@oblivon-software.de)
 *
 * Transcoder derived from this example:
 * https://fossies.org/linux/libav/doc/examples/transcode_aac.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

//#define _USE_LIBSWRESAMPLE

#include "ffmpeg_transcoder.h"

#include <algorithm>
#include <cstdlib>

#include <unistd.h>

#include <vector>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "transcode.h"
#include "buffer.h"

// Rescale ms to to timebase
// av_rescale_q(s->output_ts_offset, AV_TIME_BASE_Q, st->time_base);
// Rescan input to ouput time scale
// av_rescale_q_rnd(output_packet.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
int64_t audio_start_pts;
int64_t video_start_pts;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define INVALID_STREAM  -1

FfmpegTranscoder::FfmpegTranscoder()
    : m_nActual_size(0)
    , m_bIsVideo(false)
    #ifdef _USE_LIBSWRESAMPLE
    , m_pSwr_ctx(NULL)
    #else
    , m_pAudio_resample_ctx(NULL)
    #endif
    , m_pAudioFifo(NULL)
    , m_pSws_ctx(NULL)
    , m_pts(AV_NOPTS_VALUE)
    , m_pos(AV_NOPTS_VALUE)
    , m_in({
           .m_pFormat_ctx = NULL,
           .m_pAudio_codec_ctx = NULL,
           .m_pVideo_codec_ctx = NULL,
           .m_pAudio_stream = NULL,
           .m_pVideo_stream = NULL,
           .m_nAudio_stream_idx = INVALID_STREAM,
           .m_nVideo_stream_idx = INVALID_STREAM
        })
    , m_out({
            .m_output_type = TYPE_UNKNOWN,
            .m_pFormat_ctx = NULL,
            .m_pAudio_codec_ctx = NULL,
            .m_pVideo_codec_ctx = NULL,
            .m_pAudio_stream = NULL,
            .m_pVideo_stream = NULL,
            .m_nAudio_stream_idx = INVALID_STREAM,
            .m_nVideo_stream_idx = INVALID_STREAM,
            .m_nAudio_pts = 0,
            .m_nVideo_pts = 0,
            .m_nVideo_offset = 0,
            .m_id3v1 = {}
        })
{
    mp3fs_debug("FFMPEG trancoder: ready to initialise.");

    // Initialise ID3v1.1 tag structure
    memset(&m_out.m_id3v1, ' ', sizeof(ID3v1));
    memcpy(&m_out.m_id3v1.szTAG, "TAG", 3);
    m_out.m_id3v1.cPad = '\0';
    m_out.m_id3v1.cTitleNo = 0;
    m_out.m_id3v1.cGenre = 0;

    audio_start_pts = 0;
    video_start_pts = 0;
}

/* Free the FFMPEG en/decoder and close the open FFMPEG file
 * after the transcoding process has finished.
 */
FfmpegTranscoder::~FfmpegTranscoder() {

    // Close fifo and resample context
    if (m_pAudioFifo)
    {
        av_audio_fifo_free(m_pAudioFifo);
    }

    while (m_VideoFifo.size())
    {
        AVFrame *output_frame = m_VideoFifo.front();
        m_VideoFifo.pop();

        av_frame_free(&output_frame);
    }

#ifdef _USE_LIBSWRESAMPLE
    if (m_pSwr_ctx != NULL)
    {
        swr_free(&m_pSwr_ctx);
    }
#else
    if (m_pAudio_resample_ctx) {
        avresample_close(m_pAudio_resample_ctx);
        avresample_free(&m_pAudio_resample_ctx);
    }
#endif	

    if (m_pSws_ctx != NULL)
    {
        sws_freeContext(m_pSws_ctx);
    }

    // Close input file
    if (m_out.m_pFormat_ctx != NULL)
    {
        AVIOContext *output_io_context  = (AVIOContext *)m_out.m_pFormat_ctx->pb;

        //avio_close(output_format_context->pb);

        av_freep(&output_io_context->buffer);
        av_freep(&output_io_context);

        avformat_free_context(m_out.m_pFormat_ctx);
    }

    // Close output file

    if (m_in.m_pFormat_ctx != NULL)
    {
        avformat_close_input(&m_in.m_pFormat_ctx);
    }

    mp3fs_debug("FFMPEG trancoder: closed.");
}

/*
 * Open codec context for desired media type
 */

int FfmpegTranscoder::open_codec_context(int *stream_idx, AVCodecContext **avctx, AVFormatContext *fmt_ctx, AVMediaType type, const char *filename)
{
    int ret;

    ret = av_find_best_stream(fmt_ctx, type, INVALID_STREAM, INVALID_STREAM, NULL, 0);
    if (ret < 0) {
        mp3fs_error("FFMPEG transcoder: Could not find %s stream in input file '%s' (error '%s').", get_media_type_string(type), filename, ffmpeg_geterror(ret).c_str());
        return ret;
    } else {
        int stream_index;
        AVCodecContext *dec_ctx = NULL;
        AVCodec *dec = NULL;
        AVDictionary *opts = NULL;
        AVStream *in_stream;
        AVCodecID codec_id = AV_CODEC_ID_NONE;

        stream_index = ret;
        in_stream = fmt_ctx->streams[stream_index];

        /* Init the decoders, with or without reference counting */
        // av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);

#if (LIBAVCODEC_VERSION_MICRO >= 100 && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT( 57, 64, 101 ) )
        /** allocate a new decoding context */
        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            mp3fs_error("FFMPEG transcoder: Could not allocate a decoding context.");
            return AVERROR(ENOMEM);
        }

        /** initialise the stream parameters with demuxer information */
        ret = avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
        if (ret < 0) {
            return ret;
        }

        codec_id = in_stream->codecpar->codec_id;
#else
        dec_ctx = in_stream->codec;

        codec_id = dec_ctx->codec_id;
#endif

        /** Find a decoder for the stream. */
        dec = avcodec_find_decoder(codec_id);
        if (!dec) {
            mp3fs_error("FFMPEG transcoder: failed to find %s codec.", get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        dec_ctx->codec_id = dec->id;

        ret = avcodec_open2(dec_ctx, dec, &opts);
        if (ret < 0) {
            mp3fs_error("FFMPEG transcoder: Failed to find %s input codec (error '%s').", get_media_type_string(type), ffmpeg_geterror(ret).c_str());
            return ret;
        }

        mp3fs_debug("FFMPEG transcoder: Successfully opened %s input codec.", get_codec_name(codec_id));

        *stream_idx = stream_index;

        *avctx = dec_ctx;
    }

    return 0;
}

/*
 * FFMPEG handles cover arts like video streams.
 * Try to find out if we have a video stream or a cover art.
 */
bool FfmpegTranscoder::is_video() const
{
    bool bIsVideo = false;

    if (m_in.m_pVideo_codec_ctx != NULL && m_in.m_pVideo_stream != NULL)
    {
        if ((m_in.m_pVideo_codec_ctx->codec_id == AV_CODEC_ID_PNG) || (m_in.m_pVideo_codec_ctx->codec_id == AV_CODEC_ID_MJPEG))
        {
            bIsVideo = false;

            if (m_in.m_pVideo_stream->r_frame_rate.den)
            {
                double dbFrameRate = (double)m_in.m_pVideo_stream->r_frame_rate.num / m_in.m_pVideo_stream->r_frame_rate.den;

                // If frame rate is < 100 fps this should be a video
                if (dbFrameRate < 100)
                {
                    bIsVideo = true;
                }
            }
        }
        else
        {
            // If the source codec is not PNG or JPG we can safely assume it's a video stream
            bIsVideo = true;
        }
    }

    return bIsVideo;
}

/*
 * Open the given FFMPEG file and prepare for decoding. After this function,
 * the other methods can be used to process the file.
 */
int FfmpegTranscoder::open_file(const char* filename) {
    AVDictionary * opt = NULL;
    int ret;

    mp3fs_debug("FFMPEG transcoder: initialising.");

    struct stat s;
    if (stat(filename, &s) < 0) {
        mp3fs_error("FFMPEG transcoder: stat failed.");
        return -1;
    }
    m_mtime = s.st_mtime;

    //    This allows selecting if the demuxer should consider all streams to be
    //    found after the first PMT and add further streams during decoding or if it rather
    //    should scan all that are within the analyze-duration and other limits

    ret = ::av_dict_set(&opt, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    if (ret < 0)
    {
        mp3fs_error("FFMPEG transcoder: Error setting dictionary options file '%s' (error '%s').", filename, ffmpeg_geterror(ret).c_str());
        return -1; // Couldn't open file
    }

    /** Open the input file to read from it. */
    assert(m_in.m_pFormat_ctx == NULL);
    ret = avformat_open_input(&m_in.m_pFormat_ctx, filename, NULL, &opt);
    if (ret < 0) {
        mp3fs_error("FFMPEG transcoder: Could not open input file '%s' (error '%s').", filename, ffmpeg_geterror(ret).c_str());
        return ret;
    }

    ret = ::av_dict_set(&opt, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);
    if (ret < 0)
    {
        mp3fs_error("FFMPEG transcoder: Error setting dictionary options (error '%s').", ffmpeg_geterror(ret).c_str());
        return -1; // Couldn't open file
    }

    AVDictionaryEntry * t;
    if ((t = av_dict_get(opt, "", NULL, AV_DICT_IGNORE_SUFFIX)))
    {
        mp3fs_error("FFMPEG transcoder: Option %s not found.", t->key);
        return -1; // Couldn't open file
    }

#if (LIBAVFORMAT_VERSION_MICRO >= 100 && LIBAVFORMAT_VERSION_INT >= LIBAVCODEC_MIN_VERSION_INT )
    av_format_inject_global_side_data(m_in.m_pFormat_ctx);
#endif

    /** Get information on the input file (number of streams etc.). */
    ret = avformat_find_stream_info(m_in.m_pFormat_ctx, NULL);
    if (ret < 0) {
        mp3fs_error("FFMPEG transcoder: Could not open find stream info (error '%s').", ffmpeg_geterror(ret).c_str());
        avformat_close_input(&m_in.m_pFormat_ctx);
        m_in.m_pFormat_ctx = NULL;
        return ret;
    }

    // Open best match video codec
    ret = open_codec_context(&m_in.m_nVideo_stream_idx, &m_in.m_pVideo_codec_ctx, m_in.m_pFormat_ctx, AVMEDIA_TYPE_VIDEO, filename);
    if (ret < 0) {
        mp3fs_warning("FFMPEG transcoder: Failed to open video codec (error '%s').", ffmpeg_geterror(ret).c_str());
        //        avformat_close_input(&m_in.m_pFormat_ctx);
        //        m_in.m_pFormat_ctx = NULL;
        //        return ret;
    }

    if (m_in.m_nVideo_stream_idx >= 0) {
        // We have a video stream
        m_in.m_pVideo_stream = m_in.m_pFormat_ctx->streams[m_in.m_nVideo_stream_idx];

        m_bIsVideo = is_video();

        if(m_in.m_pVideo_codec_ctx->codec->capabilities & CODEC_CAP_TRUNCATED)
        {
            m_in.m_pVideo_codec_ctx->flags|= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
        }
    }

    // Open best match audio codec
    // Save the audio decoder context for easier access later.
    ret = open_codec_context(&m_in.m_nAudio_stream_idx, &m_in.m_pAudio_codec_ctx, m_in.m_pFormat_ctx, AVMEDIA_TYPE_AUDIO, filename);
    if (ret < 0) {
        mp3fs_warning("FFMPEG transcoder: Failed to open audio codec (error '%s').", ffmpeg_geterror(ret).c_str());
        //        avformat_close_input(&m_in.m_pFormat_ctx);
        //        m_in.m_pFormat_ctx = NULL;
        //        return ret;
    }

    if (m_in.m_nAudio_stream_idx >= 0) {
        // We have an audio stream
        m_in.m_pAudio_stream = m_in.m_pFormat_ctx->streams[m_in.m_nAudio_stream_idx];
    }

    if (m_in.m_nAudio_stream_idx == -1 && m_in.m_nVideo_stream_idx == -1) {
        mp3fs_error("FFMPEG transcoder: File contains neither a video nor an audio stream.");
        return AVERROR(EINVAL);
    }

    return 0;
}

int FfmpegTranscoder::open_out_file(Buffer *buffer, const char* type) {

    /** Open the output file for writing. */
    if (open_output_file(buffer, type)) {
        return -1;
    }

    if (m_out.m_nAudio_stream_idx > -1)
    {
        /** Initialize the resampler to be able to convert audio sample formats. */
        if (init_resampler()){
            return -1;
        }
        /** Initialize the FIFO buffer to store audio samples to be encoded. */
        if (init_fifo()){
            return -1;
        }
    }
    /** Write the header of the output file container. */
    if (write_output_file_header()){
        return -1;
    }

    return 0;
}

int FfmpegTranscoder::add_stream(AVCodecID codec_id)
{
    AVCodecContext *codec_ctx     = NULL;
    AVStream *      stream        = NULL;
    AVCodec *       output_codec  = NULL;
    AVDictionary *  opt           = NULL;
    int ret;

    /* find the encoder */
    output_codec = avcodec_find_encoder(codec_id);
    if (!output_codec) {
        mp3fs_error("FFMPEG transcoder: Could not find encoder for '%s'.", avcodec_get_name(codec_id));
        exit(1);
    }

#if (LIBAVCODEC_VERSION_MAJOR > 56) // Check for FFMPEG 3
    stream = avformat_new_stream(m_out.m_pFormat_ctx, NULL);
#else
    stream = avformat_new_stream(m_out.m_pFormat_ctx, output_codec);
#endif
    if (!stream) {
        mp3fs_error("FFMPEG transcoder: Could not allocate stream.");
        return AVERROR(ENOMEM);
    }
    stream->id = m_out.m_pFormat_ctx->nb_streams-1;
#if (LIBAVCODEC_VERSION_MAJOR > 56) // Check for FFMPEG 3
    codec_ctx = avcodec_alloc_context3(output_codec);
    if (!codec_ctx) {
        mp3fs_error("FFMPEG transcoder: Could not alloc an encoding context.");
        return AVERROR(ENOMEM);
    }
#else
    codec_ctx = stream->codec;
#endif

    switch (output_codec->type) {
    case AVMEDIA_TYPE_AUDIO:
    {
        /**
         * Set the basic encoder parameters.
         * The input file's sample rate is used to avoid a sample rate conversion.
         */
        codec_ctx->channels               = 2;
        codec_ctx->channel_layout         = av_get_default_channel_layout(codec_ctx->channels);
        codec_ctx->sample_rate            = m_in.m_pAudio_codec_ctx->sample_rate;
        codec_ctx->sample_fmt             = output_codec->sample_fmts[0];
        codec_ctx->bit_rate               = params.audiobitrate * 1000;

        /** Allow the use of the experimental AAC encoder */
        codec_ctx->strict_std_compliance  = FF_COMPLIANCE_EXPERIMENTAL;

        /** Set the sample rate for the container. */
        stream->time_base.den             = m_in.m_pAudio_codec_ctx->sample_rate;
        stream->time_base.num             = 1;

#if (LIBAVCODEC_VERSION_MAJOR <= 56) // Check for FFMPEG 3
        // set -strict -2 for aac (required for FFMPEG 2)
        av_dict_set(&opt, "strict", "-2", 0);
#endif

        /** Save the encoder context for easier access later. */
        m_out.m_pAudio_codec_ctx    = codec_ctx;
        // Save the stream index
        m_out.m_nAudio_stream_idx = stream->index;
        // Save output audio stream for faster reference
        m_out.m_pAudio_stream = stream;
        break;
    }
    case AVMEDIA_TYPE_VIDEO:
    {
        codec_ctx->codec_id = codec_id;

        /**
         * Set the basic encoder parameters.
         * The input file's sample rate is used to avoid a sample rate conversion.
         */
        AVStream *in_video_stream = m_in.m_pVideo_stream;

        //        ret = avcodec_parameters_from_context(stream->codecpar, m_in.m_pVideo_codec_ctx);
        //        if (ret < 0) {
        //            return ret;
        //        }

        int64_t bit_rate = 1000*1000;// params.videobitrate * 1000;

        if (in_video_stream->codec->bit_rate < bit_rate)
        {
            bit_rate = in_video_stream->codec->bit_rate;
        }

        codec_ctx->bit_rate             = bit_rate;
        codec_ctx->bit_rate_tolerance   = 0;
        /* Resolution must be a multiple of two. */
        codec_ctx->width                = in_video_stream->codec->width;
        codec_ctx->height               = in_video_stream->codec->height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        stream->time_base               = in_video_stream->time_base;
        codec_ctx->time_base            = stream->time_base;
        // At this moment the output format must be AV_PIX_FMT_YUV420P;
        codec_ctx->pix_fmt       		= AV_PIX_FMT_YUV420P;

        if (in_video_stream->codec->pix_fmt != codec_ctx->pix_fmt ||
                in_video_stream->codec->width != codec_ctx->width ||
                in_video_stream->codec->height != codec_ctx->height)
        {
            // Rescal image if required
            const AVPixFmtDescriptor *fmtin = av_pix_fmt_desc_get((AVPixelFormat)in_video_stream->codec->pix_fmt);
            const AVPixFmtDescriptor *fmtout = av_pix_fmt_desc_get(codec_ctx->pix_fmt);
            mp3fs_debug("FFMPEG transcoder: Initialising pixel format conversion %s to %s.", fmtin != NULL ? fmtin->name : "-", fmtout != NULL ? fmtout->name : "-");
            assert(m_pSws_ctx == NULL);
            m_pSws_ctx = sws_getContext(
                        // Source settings
                        in_video_stream->codec->width,          // width
                        in_video_stream->codec->height,         // height
                        in_video_stream->codec->pix_fmt,        // format
                        // Target settings
                        codec_ctx->width,                       // width
                        codec_ctx->height,                      // height
                        codec_ctx->pix_fmt,                     // format
                        SWS_BICUBIC, NULL, NULL, NULL);
            if (!m_pSws_ctx)
            {
                mp3fs_error("FFMPEG transcoder: Could not allocate scaling/conversion context.");
                return AVERROR(ENOMEM);
            }
        }

        codec_ctx->gop_size             = 12;   // emit one intra frame every twelve frames at most

        codec_ctx->framerate            = in_video_stream->codec->framerate;
        codec_ctx->sample_aspect_ratio  = in_video_stream->codec->sample_aspect_ratio;
        av_opt_set(codec_ctx->priv_data, "profile", "baseline", AV_OPT_SEARCH_CHILDREN);
        av_opt_set(codec_ctx->priv_data, "preset", "veryfast", AV_OPT_SEARCH_CHILDREN);
        /** Save the encoder context for easier access later. */
        m_out.m_pVideo_codec_ctx    = codec_ctx;
        // Save the stream index
        m_out.m_nVideo_stream_idx = stream->index;
        // Save output video stream for faster reference
        m_out.m_pVideo_stream = stream;
        break;
    }
    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (m_out.m_pFormat_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    if ((ret = avcodec_open2(codec_ctx, output_codec, &opt)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not open audio output codec (error '%s').", ffmpeg_geterror(ret).c_str());
        avcodec_free_context(&codec_ctx);
        return ret;
    }

    mp3fs_debug("FFMPEG transcoder: successfully opened %s output codec.", get_codec_name(codec_id));

#if (LIBAVCODEC_VERSION_MAJOR > 56) // Check for FFMPEG 3
    ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
    if (ret < 0) {
        mp3fs_error("FFMPEG transcoder: Could not initialise stream parameters (error '%s').", ffmpeg_geterror(ret).c_str());
        avcodec_free_context(&codec_ctx);
        return ret;
    }
#endif

    return 0;
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 */
int FfmpegTranscoder::open_output_file(Buffer *buffer, const char* type)
{
    AVIOContext *   output_io_context   = NULL;
    AVCodecID       audio_codecid;
    AVCodecID       video_codecid;
    const char *    ext;
    int             ret                 = 0;

    if (!strcasecmp(type, "mp3"))
    {
        ext = "mp3";
        audio_codecid = AV_CODEC_ID_MP3;
        video_codecid = AV_CODEC_ID_PNG;
        m_out.m_output_type = TYPE_MP3;
    }
    else if (!strcasecmp(type, "mp4"))
    {
        ext = "mp4";
        audio_codecid = AV_CODEC_ID_AAC;
        video_codecid = AV_CODEC_ID_H264;
        // video_codecid = AV_CODEC_ID_MJPEG;
        m_out.m_output_type = TYPE_MP4;
    }
    else if (!strcasecmp(type, "ismv"))
    {
        ext = "ISMV";
        audio_codecid = AV_CODEC_ID_AAC;
        video_codecid = AV_CODEC_ID_H264;
        m_out.m_output_type = TYPE_ISMV;
    }
    else
    {
        mp3fs_error("FFMPEG transcoder: Unknown format type \"%s\"", type);
        return -1;
    }

    mp3fs_debug("FFMPEG transcoder: Opening format type \"%s\"", type);

    /** Create a new format context for the output container format. */
    avformat_alloc_output_context2(&m_out.m_pFormat_ctx, NULL, ext, NULL);
    //m_out.m_pFormat_ctx = avformat_alloc_context();
    if (!m_out.m_pFormat_ctx) {
        mp3fs_error("FFMPEG transcoder: Could not allocate output format context.");
        return AVERROR(ENOMEM);
    }

    if (!m_bIsVideo)
    {
        m_in.m_nVideo_stream_idx = INVALID_STREAM;  // TEST
    }

    //video_codecid = m_out.m_pFormat_ctx->oformat->video_codec;

    if (m_in.m_nVideo_stream_idx != INVALID_STREAM)
    {
        ret = add_stream(video_codecid);
        if (ret < 0) {
            return ret;
        }
    }

    //    m_in.m_nAudio_stream_idx = INVALID_STREAM;

    //audio_codecid = m_out.m_pFormat_ctx->oformat->audio_codec;

    if (m_in.m_nAudio_stream_idx != INVALID_STREAM)
    {
        ret = add_stream(audio_codecid);
        if (ret < 0) {
            return ret;
        }
    }

    /* open the output file */
    int nBufSize = 1024;
    output_io_context = ::avio_alloc_context(
                (unsigned char *) ::av_malloc(nBufSize + FF_INPUT_BUFFER_PADDING_SIZE),
                nBufSize,
                1,
                (void *)buffer,
                NULL /*readPacket*/,
                writePacket,
                seek);

    /** Associate the output file (pointer) with the container format context. */
    m_out.m_pFormat_ctx->pb = output_io_context;

    if (/*!(m_out.m_pFormat_ctx->oformat->flags & (AVFMT_TS_NEGATIVE | AVFMT_NOTIMESTAMPS)) &&*/
            (m_in.m_nVideo_stream_idx != INVALID_STREAM) &&
            (m_in.m_nAudio_stream_idx != INVALID_STREAM))
    {
        // Calculate difference
        AVStream *in_audio_stream = m_in.m_pAudio_stream;
        AVStream *in_video_stream = m_in.m_pVideo_stream;

        int64_t audio_start;

        audio_start = av_rescale_q(in_audio_stream->start_time, in_audio_stream->time_base, AV_TIME_BASE_Q);

        video_start_pts = av_rescale_q(audio_start /*diff*/, AV_TIME_BASE_Q, in_video_stream->time_base);

        m_out.m_nVideo_offset = video_start_pts;
   }

    return 0;
}

/** Initialize one data packet for reading or writing. */
void FfmpegTranscoder::init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /** Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

/** Initialize one audio frame for reading from the input file */
int FfmpegTranscoder::init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        mp3fs_error("FFMPEG transcoder: Could not allocate input frame.");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
         * Initialize the audio resampler based on the input and output codec settings.
         * If the input and output sample formats differ, a conversion is required
         * libavresample takes care of this, but requires initialisation.
         */
int FfmpegTranscoder::init_resampler()
{
    /**
      * Only initialise the resampler if it is necessary, i.e.,
      * if and only if the sample formats differ.
      */
    if (m_in.m_pAudio_codec_ctx->sample_fmt != m_out.m_pAudio_codec_ctx->sample_fmt ||
            m_in.m_pAudio_codec_ctx->sample_rate != m_out.m_pAudio_codec_ctx->sample_rate ||
            m_in.m_pAudio_codec_ctx->channels != m_out.m_pAudio_codec_ctx->channels) {
        int ret;

#ifdef _USE_LIBSWRESAMPLE
        /* create resampler context */
        m_pSwr_ctx = swr_alloc();
        if (!m_pSwr_ctx) {
            mp3fs_error("FFMPEG transcoder: Could not allocate resampler context.");
            return AVERROR(ENOMEM);
        }

        /* set options */
        /**
          * Set the conversion parameters.
          * Default channel layouts based on the number of channels
          * are assumed for simplicity (they are sometimes not detected
          * properly by the demuxer and/or decoder).
          */
        av_opt_set_int       (m_pSwr_ctx, "in_channel_count",   av_get_default_channel_layout(m_in.m_pAudio_codec_ctx->channels),  0);
        av_opt_set_int       (m_pSwr_ctx, "out_channel_count",  av_get_default_channel_layout(m_out.m_pAudio_codec_ctx->channels), 0);
        av_opt_set_int       (m_pSwr_ctx, "in_sample_rate",     m_in.m_pAudio_codec_ctx->sample_rate,                              0);
        av_opt_set_int       (m_pSwr_ctx, "out_sample_rate",    m_out.m_pAudio_codec_ctx->sample_rate,                             0);
        av_opt_set_sample_fmt(m_pSwr_ctx, "in_sample_fmt",      m_in.m_pAudio_codec_ctx->sample_fmt,                               0);
        av_opt_set_sample_fmt(m_pSwr_ctx, "out_sample_fmt",     m_out.m_pAudio_codec_ctx->sample_fmt,                              0);

        /* initialise the resampling context */
        if ((ret = swr_init(m_pSwr_ctx)) < 0) {
            mp3fs_error("FFMPEG transcoder: Could not open resampler context (error '%s').", ffmpeg_geterror(ret).c_str());
            swr_free(&m_pSwr_ctx);
            m_pSwr_ctx = NULL;
            return ret;
        }
#else
        /** Create a resampler context for the conversion. */
        if (!(m_pAudio_resample_ctx = avresample_alloc_context())) {
            mp3fs_error("FFMPEG transcoder: Could not allocate resample context");
            return AVERROR(ENOMEM);
        }

        /**
                 * Set the conversion parameters.
                 * Default channel layouts based on the number of channels
                 * are assumed for simplicity (they are sometimes not detected
                 * properly by the demuxer and/or decoder).
                 */
        av_opt_set_int(m_pAudio_resample_ctx, "in_channel_layout", av_get_default_channel_layout(m_in.m_pAudio_codec_ctx->channels), 0);
        av_opt_set_int(m_pAudio_resample_ctx, "out_channel_layout", av_get_default_channel_layout(m_out.m_pAudio_codec_ctx->channels), 0);
        av_opt_set_int(m_pAudio_resample_ctx, "in_sample_rate", m_in.m_pAudio_codec_ctx->sample_rate, 0);
        av_opt_set_int(m_pAudio_resample_ctx, "out_sample_rate", m_out.m_pAudio_codec_ctx->sample_rate, 0);
        av_opt_set_int(m_pAudio_resample_ctx, "in_sample_fmt", m_in.m_pAudio_codec_ctx->sample_fmt, 0);
        av_opt_set_int(m_pAudio_resample_ctx, "out_sample_fmt", m_out.m_pAudio_codec_ctx->sample_fmt, 0);

        /** Open the resampler with the specified parameters. */
        if ((ret = avresample_open(m_pAudio_resample_ctx)) < 0) {
            mp3fs_error("FFMPEG transcoder: Could not open resampler context (error '%s').", ffmpeg_geterror(ret).c_str());
            avresample_free(&m_pAudio_resample_ctx);
            m_pAudio_resample_ctx = NULL;
            return ret;
        }
#endif	

    }
    return 0;
}

/** Initialize a FIFO buffer for the audio samples to be encoded. */
int FfmpegTranscoder::init_fifo()
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(m_pAudioFifo = av_audio_fifo_alloc(m_out.m_pAudio_codec_ctx->sample_fmt, m_out.m_pAudio_codec_ctx->channels, 1))) {
        mp3fs_error("FFMPEG transcoder: Could not allocate FIFO");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/** Write the header of the output file container. */
int FfmpegTranscoder::write_output_file_header()
{
    int error;
    AVDictionary* dict = nullptr;

    if (m_out.m_output_type == TYPE_MP4)
    {
        // Settings for fast playback start in HTML5
        av_dict_set(&dict, "movflags", "faststart", 0);
        av_dict_set(&dict, "movflags", "empty_moov", 0);
        av_dict_set(&dict, "frag_duration", "1000000", 0); // 1 sec
    }

    if ((error = avformat_write_header(m_out.m_pFormat_ctx, &dict)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not write output file header (error '%s').", ffmpeg_geterror(error).c_str());
        return error;
    }
    return 0;
}

AVFrame *FfmpegTranscoder::alloc_picture(AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        mp3fs_error("FFMPEG transcoder: Could not allocate frame data.");
        av_frame_free(&picture);
        return NULL;
    }

    return picture;
}

/** Decode one audio frame from the input file. */
int FfmpegTranscoder::decode_frame(AVPacket *input_packet, int *data_present)
{
    int decoded = 0;

    if (input_packet->stream_index == m_in.m_nAudio_stream_idx)
    {
        /** Temporary storage of the input samples of the frame read from the file. */
        AVFrame *input_frame = NULL;
        int ret = 0;

        /** Initialize temporary storage for one input frame. */
        ret = init_input_frame(&input_frame);
        if (ret < 0)
        {
            av_frame_free(&input_frame);
            return ret;
        }

        /**
                * Decode the audio frame stored in the temporary packet.
                * The input audio stream decoder is used to do this.
                * If we are at the end of the file, pass an empty packet to the decoder
                * to flush it.
                */
        ret = avcodec_decode_audio4(m_in.m_pAudio_codec_ctx, input_frame, data_present, input_packet);
        if (ret < 0 && ret != AVERROR_INVALIDDATA) {
            av_frame_free(&input_frame);
            mp3fs_error("FFMPEG transcoder: Could not decode audio frame (error '%s').", ffmpeg_geterror(ret).c_str());
            return ret;
        }

        if (ret != AVERROR_INVALIDDATA)
        {
            decoded = ret;
        }
        else
        {
            decoded = input_packet->size;
        }

        /** If there is decoded data, convert and store it */
        if (data_present && input_frame->nb_samples) {
            /** Temporary storage for the converted input samples. */
            uint8_t **converted_input_samples = NULL;

            // Store audio frame
            /** Initialize the temporary storage for the converted input samples. */
            ret = init_converted_samples(&converted_input_samples, input_frame->nb_samples);
            if (ret < 0)
            {
                goto cleanup2;
            }

            /**
                 * Convert the input samples to the desired output sample format.
                 * This requires a temporary storage provided by converted_input_samples.
                 */
            ret = convert_samples(input_frame->extended_data, converted_input_samples, input_frame->nb_samples);
            if (ret < 0)
            {
                goto cleanup2;
            }

            /** Add the converted input samples to the FIFO buffer for later processing. */
            ret = add_samples_to_fifo(converted_input_samples, input_frame->nb_samples);
            if (ret < 0)
            {
                goto cleanup2;
            }
            ret = 0;
cleanup2:
            if (converted_input_samples) {
                av_freep(&converted_input_samples[0]);
                free(converted_input_samples);
            }
        }

        //cleanup:

        av_frame_free(&input_frame);
    }
    else if (input_packet->stream_index == m_in.m_nVideo_stream_idx)
    {
        AVFrame *input_frame = NULL;
        int ret = 0;

        /** Initialize temporary storage for one input frame. */
        ret = init_input_frame(&input_frame);
        if (ret < 0)
            return ret;

        /* NOTE1: some codecs are stream based (mpegvideo, mpegaudio)
           and this is the only method to use them because you cannot
           know the compressed data size before analysing it.

           BUT some other codecs (msmpeg4, mpeg4) are inherently frame
           based, so you must call them with all the data for one
           frame exactly. You must also initialise 'width' and
           'height' before initialising them. */

        /* NOTE2: some codecs allow the raw parameters (frame size,
           sample rate) to be changed at any frame. We handle this, so
           you should also take care of it */

        /* here, we use a stream based decoder (mpeg1video), so we
           feed decoder and see if it could decode a frame */

        ret = avcodec_decode_video2(m_in.m_pVideo_codec_ctx, input_frame, data_present, input_packet);

//        if (ret == -1 || ret == AVERROR_INVALIDDATA)
//        {
//            // unused frame
//            av_frame_free(&input_frame);
//            return 0;
//        }

        if (ret < 0) {
            mp3fs_error("FFMPEG transcoder: Could not decode video frame (error '%s').", ffmpeg_geterror(ret).c_str());
            // unused frame
            av_frame_free(&input_frame);
            return ret;
        }

        decoded = ret;

        // Sometimes only a few packets contain valid dts/pts/pos data, so we keep it
        // TODO: pFrame->opaque unused???
        if (input_packet->dts == AV_NOPTS_VALUE && input_frame->opaque && *(int64_t *)input_frame->opaque != AV_NOPTS_VALUE)
        {
            m_pts = *(uint64_t *)input_frame->opaque;
        }

        else if (input_packet->dts != AV_NOPTS_VALUE)
        {
            m_pts = input_packet->dts;
        }
        else if (input_packet->pts != AV_NOPTS_VALUE)
        {
            m_pts = input_packet->pts;
        }

        if (input_packet->pos > -1)
        {
            m_pos = input_packet->pos;
        }

        if (*data_present)
        {
            if (m_pts == AV_NOPTS_VALUE)
            {
                m_pts = 0;
            }

            if (m_pos == AV_NOPTS_VALUE)
            {
                m_pos = 0;
            }

            if (m_pSws_ctx != NULL)
            {
                AVCodecContext *c = m_out.m_pVideo_codec_ctx;

                AVFrame * tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
                if (!tmp_frame) {
                    return AVERROR(ENOMEM);
                }

                sws_scale(m_pSws_ctx,
                          (const uint8_t * const *)input_frame->data, input_frame->linesize,
                          0, c->height,
                          tmp_frame->data, tmp_frame->linesize);

                tmp_frame->pts = input_frame->pts;

                av_frame_free(&input_frame);

                input_frame = tmp_frame;
            }

            // AV_NOPTS_VALUE
            //int i64Pts3 = ::av_rescale(AV_TIME_BASE * pkt.pts, m_pVideoStream->codecpar->time_base.num, m_pVideoStream->codecpar->time_base.den);
            //i64Pts = (i64Pts * 1000000LL * m_pVideoStream->time_base.num) / m_pVideoStream->time_base.den;
            //m_pts = ::av_rescale(AV_TIME_BASE * m_pts, m_in.m_pVideo_stream->time_base.num, m_in.m_pVideo_stream->time_base.den);

            input_frame->pts = av_rescale_q_rnd(m_pts, m_in.m_pVideo_stream->time_base, m_out.m_pVideo_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            //input_frame->pts = m_pts;

            //input_frame->pts = input_packet->pts;
            /* the picture is allocated by the decoder. no need to free it */
            // Store video frame
            //input_frame->pts = av_frame_get_best_effort_timestamp(input_frame); // ???
            //            input_frame->pts = input_packet->dts;  // ???
            m_VideoFifo.push(input_frame);
        }
        else
        {
            // unused frame
            av_frame_free(&input_frame);
        }
    }
    else
    {
        *data_present = 0;
        decoded = input_packet->size;    // Ignore
    }

    return decoded;
}

/**
         * Initialize a temporary storage for the specified number of audio samples.
         * The conversion requires temporary storage due to the different format.
         * The number of audio samples to be allocated is specified in frame_size.
         */
int FfmpegTranscoder::init_converted_samples(uint8_t ***converted_input_samples, int frame_size)
{
    int error;

    /**
             * Allocate as many pointers as there are audio channels.
             * Each pointer will later point to the audio samples of the corresponding
             * channels (although it may be NULL for interleaved formats).
             */
    if (!(*converted_input_samples = (uint8_t **)calloc(m_out.m_pAudio_codec_ctx->channels, sizeof(**converted_input_samples)))) {
        mp3fs_error("FFMPEG transcoder: Could not allocate converted input sample pointers");
        return AVERROR(ENOMEM);
    }

    /**
             * Allocate memory for the samples of all channels in one consecutive
             * block for convenience.
             */
    if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                  m_out.m_pAudio_codec_ctx->channels,
                                  frame_size,
                                  m_out.m_pAudio_codec_ctx->sample_fmt, 0)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not allocate converted input samples (error '%s').", ffmpeg_geterror(error).c_str());
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return error;
    }
    return 0;
}

/**
         * Convert the input audio samples into the output sample format.
         * The conversion happens on a per-frame basis, the size of which is specified
         * by frame_size.
         */
int FfmpegTranscoder::convert_samples(uint8_t **input_data, uint8_t **converted_data, const int frame_size)
{
#ifdef _USE_LIBSWRESAMPLE
    if (m_pSwr_ctx != NULL)
    {
        int ret;

        /** Convert the samples using the resampler. */

        //avresample_convert(m_pAudio_resample_ctx, converted_data, 0, frame_size, (const uint8_t **)input_data, 0, frame_size)
        ret = swr_convert(m_pSwr_ctx, converted_data, frame_size, (const uint8_t **)input_data, frame_size);
        if (ret < 0) {
            mp3fs_error("FFMPEG transcoder: Could not convert input samples (error '%s').", ffmpeg_geterror(ret).c_str());
            return ret;
        }


    }
#else
    if (m_pAudio_resample_ctx != NULL)
    {
        int ret;

        /** Convert the samples using the resampler. */
        if ((ret = avresample_convert(m_pAudio_resample_ctx, converted_data, 0, frame_size, input_data, 0, frame_size)) < 0) {
            mp3fs_error("FFMPEG transcoder: Could not convert input samples (error '%s').", ffmpeg_geterror(ret).c_str());
            return ret;
        }

        /**
             * Perform a sanity check so that the number of converted samples is
             * not greater than the number of samples to be converted.
             * If the sample rates differ, this case has to be handled differently
             */
        if (avresample_available(m_pAudio_resample_ctx)) {
            mp3fs_error("FFMPEG transcoder: Converted samples left over");
            return AVERROR_EXIT;
        }
    }
#endif
    else
    {
        // No resampling, just copy samples
        if (!av_sample_fmt_is_planar(m_in.m_pAudio_codec_ctx->sample_fmt))
        {
            memcpy(converted_data[0], input_data[0], frame_size * av_get_bytes_per_sample(m_out.m_pAudio_codec_ctx->sample_fmt));
        }
        else
        {
            for (int n = 0; n < m_in.m_pAudio_codec_ctx->channels; n++)
            {
                memcpy(converted_data[n], input_data[n], frame_size * av_get_bytes_per_sample(m_out.m_pAudio_codec_ctx->sample_fmt));
            }
        }
    }
    return 0;
}

/** Add converted input audio samples to the FIFO buffer for later processing. */
int FfmpegTranscoder::add_samples_to_fifo(uint8_t **converted_input_samples, const int frame_size)
{
    int error;

    /**
             * Make the FIFO as large as it needs to be to hold both,
             * the old and the new samples.
             */
    if ((error = av_audio_fifo_realloc(m_pAudioFifo, av_audio_fifo_size(m_pAudioFifo) + frame_size)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not reallocate FIFO");
        return error;
    }

    /** Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(m_pAudioFifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        mp3fs_error("FFMPEG transcoder: Could not write data to FIFO");
        return AVERROR_EXIT;
    }
    return 0;
}

/**
         * Read one audio frame from the input file, decodes, converts and stores
         * it in the FIFO buffer.
         */
int FfmpegTranscoder::read_decode_convert_and_store(int *finished)
{
    /** Packet used for temporary storage. */
    AVPacket input_packet;
    int data_present;
    int ret = AVERROR_EXIT;

    init_packet(&input_packet);

    /** Read one audio frame from the input file into a temporary packet. */
    if ((ret = av_read_frame(m_in.m_pFormat_ctx, &input_packet)) < 0) {
        /** If we are the the end of the file, flush the decoder below. */
        if (ret == AVERROR_EOF)
        {
            *finished = 1;
        }
        else
        {
            mp3fs_error("FFMPEG transcoder: Could not read frame (error '%s').", ffmpeg_geterror(ret).c_str());
            goto cleanup;
        }
    }

    //    if (!*finished)
    {
        do {
            /** Decode one frame. */
            ret = decode_frame(&input_packet, &data_present);
            if (ret < 0)
            {
                goto cleanup;
            }
            input_packet.data += ret;
            input_packet.size -= ret;
        } while (input_packet.size > 0);
    }

    /* flush cached frames */
    //    input_packet.data = NULL;
    //    input_packet.size = 0;
    //    do {
    //        ret = decode_frame(&input_packet, &data_present);
    //        if (ret < 0)
    //            goto cleanup;
    //    } while (data_present);

    /**
             * If the decoder has not been flushed completely, we are not finished,
             * so that this function has to be called again.
             */
    if (*finished && data_present)
        *finished = 0;

    /**
             * If we are at the end of the file and there are no more samples
             * in the decoder which are delayed, we are actually finished.
             * This must not be treated as an error.
             */
    if (*finished && !data_present) {
        ret = 0;
        goto cleanup;
    }

    ret = 0;

cleanup:
    av_packet_unref(&input_packet);

    return ret;
}

/**
         * Initialize one input frame for writing to the output file.
         * The frame will be exactly frame_size samples large.
         */
int FfmpegTranscoder::init_audio_output_frame(AVFrame **frame, int frame_size)
{
    int error;

    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        mp3fs_error("FFMPEG transcoder: Could not allocate output frame");
        return AVERROR_EXIT;
    }

    /**
             * Set the frame's parameters, especially its size and format.
             * av_frame_get_buffer needs this to allocate memory for the
             * audio samples of the frame.
             * Default channel layouts based on the number of channels
             * are assumed for simplicity.
             */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = m_out.m_pAudio_codec_ctx->channel_layout;
    (*frame)->format         = m_out.m_pAudio_codec_ctx->sample_fmt;
    (*frame)->sample_rate    = m_out.m_pAudio_codec_ctx->sample_rate;

    /**
             * Allocate the samples of the created frame. This call will make
             * sure that the audio frame can hold as many samples as specified.
             */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could allocate output frame samples (error '%s').", ffmpeg_geterror(error).c_str());
        av_frame_free(frame);
        return error;
    }

    return 0;
}

void FfmpegTranscoder::produce_dts(AVPacket *pkt, int64_t *pts)
{
    //    if ((pkt->pts == 0 || pkt->pts == AV_NOPTS_VALUE) && pkt->dts == AV_NOPTS_VALUE)
    {
        int64_t duration;
        // Some encoders to not produce dts/pts.
        // So we make some up.
        assert(pkt->duration > 0);
        if (pkt->duration)
        {
            duration = pkt->duration;
        }
        else
        {
            duration = 1;
        }

        pkt->dts = *pts; // - duration;
        pkt->pts = *pts;

        *pts += duration;
    }
}
/** Encode one frame worth of audio to the output file. */
int FfmpegTranscoder::encode_audio_frame(AVFrame *frame, int *data_present)
{
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    int error;
    init_packet(&output_packet);

    /**
             * Encode the audio frame and store it in the temporary packet.
             * The output audio stream encoder is used to do this.
             */
    if ((error = avcodec_encode_audio2(m_out.m_pAudio_codec_ctx, &output_packet, frame, data_present)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not encode audio frame (error '%s').", ffmpeg_geterror(error).c_str());
        av_packet_unref(&output_packet);
        return error;
    }

    /** Write one audio frame from the temporary packet to the output file. */
    if (*data_present) {
        output_packet.stream_index = m_out.m_nAudio_stream_idx;

        produce_dts(&output_packet, &m_out.m_nAudio_pts);

        if ((error = av_interleaved_write_frame(m_out.m_pFormat_ctx, &output_packet)) < 0) {
            mp3fs_error("FFMPEG transcoder: Could not write audio frame (error '%s').", ffmpeg_geterror(error).c_str());
            av_packet_unref(&output_packet);
            return error;
        }

        av_packet_unref(&output_packet);
    }

    return 0;
}

/** Encode one frame worth of audio to the output file. */
int FfmpegTranscoder::encode_video_frame(AVFrame *frame, int *data_present)
{
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    int error;
    init_packet(&output_packet);

    /**
             * Encode the video frame and store it in the temporary packet.
             * The output video stream encoder is used to do this.
             */
    if ((error = avcodec_encode_video2(m_out.m_pVideo_codec_ctx, &output_packet, frame, data_present)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not encode video frame (error '%s').", ffmpeg_geterror(error).c_str());
        av_packet_unref(&output_packet);
        return error;
    }

    /** Write one audio frame from the temporary packet to the output file. */
    if (*data_present) {

        output_packet.pts -= m_out.m_nVideo_offset;
        output_packet.dts -= m_out.m_nVideo_offset;

        //        if (output_packet.pts >= 0)
        {
            error = av_interleaved_write_frame(m_out.m_pFormat_ctx, &output_packet);
            if (error < 0) {
                mp3fs_error("FFMPEG transcoder: Could not write video frame (error '%s').", ffmpeg_geterror(error).c_str());
                av_packet_unref(&output_packet);
                return error;
            }
        }

        av_packet_unref(&output_packet);
    }

    return 0;
}

/**
         * Load one audio frame from the FIFO buffer, encode and write it to the
         * output file.
         */
int FfmpegTranscoder::load_encode_and_write()
{
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /**
             * Use the maximum number of possible samples per frame.
             * If there is less than the maximum possible frame size in the FIFO
             * buffer use this number. Otherwise, use the maximum possible frame size
             */
    const int frame_size = FFMIN(av_audio_fifo_size(m_pAudioFifo), m_out.m_pAudio_codec_ctx->frame_size);
    int data_written;

    /** Initialize temporary storage for one output frame. */
    if (init_audio_output_frame(&output_frame, frame_size))
        return AVERROR_EXIT;

    /**
             * Read as many samples from the FIFO buffer as required to fill the frame.
             * The samples are stored in the frame temporarily.
             */
    if (av_audio_fifo_read(m_pAudioFifo, (void **)output_frame->data, frame_size) < frame_size) {
        mp3fs_error("FFMPEG transcoder: Could not read data from FIFO");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /** Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

/** Write the trailer of the output file container. */
int FfmpegTranscoder::write_output_file_trailer()
{
    int error;
    if ((error = av_write_trailer(m_out.m_pFormat_ctx)) < 0) {
        mp3fs_error("FFMPEG transcoder: Could not write output file trailer (error '%s').", ffmpeg_geterror(error).c_str());
        return error;
    }
    return 0;
}

time_t FfmpegTranscoder::mtime() {
    return m_mtime;
}

/*
         * Process the metadata in the FFMPEG file. This should be called at the
         * beginning, before reading audio data. The set_text_tag() and
         * set_picture_tag() methods of the given Encoder will be used to set the
         * metadata, with results going into the given Buffer. This function will also
         * read the actual PCM stream parameters.
         */

#define tagcpy(dst, src)    \
    for (char *p1 = (dst), *pend = p1 + sizeof(dst), *p2 = (src); *p2 && p1 < pend; p1++, p2++) \
    *p1 = *p2;

int FfmpegTranscoder::process_metadata() {

    mp3fs_debug("FFMPEG transcoder: processing metadata");

    AVDictionaryEntry *tag = NULL;

    while ((tag = av_dict_get(m_in.m_pFormat_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
    {
        av_dict_set(&m_out.m_pFormat_ctx->metadata, tag->key, tag->value, 0);

        if (m_out.m_output_type == TYPE_MP3)
        {
            if (!strcasecmp(tag->key, "ARTIST"))
            {
                tagcpy(m_out.m_id3v1.szSongArtist, tag->value);
            }
            else if (!strcasecmp(tag->key, "TITLE"))
            {
                tagcpy(m_out.m_id3v1.szSongTitle, tag->value);
            }
            else if (!strcasecmp(tag->key, "ALBUM"))
            {
                tagcpy(m_out.m_id3v1.szAlbumName, tag->value);
            }
            else if (!strcasecmp(tag->key, "COMMENT"))
            {
                tagcpy(m_out.m_id3v1.szComment, tag->value);
            }
            else if (!strcasecmp(tag->key, "YEAR") || !strcasecmp(tag->key, "DATE"))
            {
                tagcpy(m_out.m_id3v1.szYear, tag->value);
            }
            else if (!strcasecmp(tag->key, "TRACK"))
            {
                m_out.m_id3v1.cTitleNo = (char)atoi(tag->value);
            }
        }
    }

    // Pictures later. More complicated...

    return 0;
}

/*
         * Process a single frame of audio data. The encode_pcm_data() method
         * of the Encoder will be used to process the resulting audio data, with the
         * result going into the given Buffer.
         *
         * Returns:
         *  0   if decoding was OK
         *  1   if EOF reached
         *  -1  error
         */
int FfmpegTranscoder::process_single_fr() {
    int ret = 0;

    if (m_out.m_nAudio_stream_idx > -1)
    {
        /** Use the encoder's desired frame size for processing. */
        const int output_frame_size = m_out.m_pAudio_codec_ctx->frame_size;
        int finished                = 0;

        /**
                 * Make sure that there is one frame worth of samples in the FIFO
                 * buffer so that the encoder can do its work.
                 * Since the decoder's and the encoder's frame size may differ, we
                 * need to FIFO buffer to store as many frames worth of input samples
                 * that they make up at least one frame worth of output samples.
                 */
        while (av_audio_fifo_size(m_pAudioFifo) < output_frame_size) {
            /**
                     * Decode one frame worth of audio samples, convert it to the
                     * output sample format and put it into the FIFO buffer.
                     */
            if (read_decode_convert_and_store(&finished))
                goto cleanup;

            /**
                     * If we are at the end of the input file, we continue
                     * encoding the remaining audio samples to the output file.
                     */
            if (finished)
                break;
        }

        /**
                 * If we have enough samples for the encoder, we encode them.
                 * At the end of the file, we pass the remaining samples to
                 * the encoder.
                 */
        while (av_audio_fifo_size(m_pAudioFifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(m_pAudioFifo) > 0))
            /**
                     * Take one frame worth of audio samples from the FIFO buffer,
                     * encode it and write it to the output file.
                     */
            if (load_encode_and_write())
                goto cleanup;

        /**
                 * If we are at the end of the input file and have encoded
                 * all remaining samples, we can exit this loop and finish.
                 */
        if (finished) {
            int data_written;
            /** Flush the encoder as it may have delayed frames. */
            do {
                if (encode_audio_frame(NULL, &data_written))
                    goto cleanup;
            } while (data_written);
            ret = 1;
        }
    }
    else
    {
        int finished                = 0;

        if (read_decode_convert_and_store(&finished))
            goto cleanup;

        if (finished) {
            ret = 1;
        }
    }

    while (!m_VideoFifo.empty())
    {
        AVFrame *output_frame = m_VideoFifo.front();
        m_VideoFifo.pop();

        /** Encode one video frame. */
        int data_written;
        output_frame->key_frame = 0;
        if (encode_video_frame(output_frame, &data_written)) {
            av_frame_free(&output_frame);
            goto cleanup;
        }
        av_frame_free(&output_frame);

        /**
                         * If we are at the end of the input file and have encoded
                         * all remaining samples, we can exit this loop and finish.
                         */
        //        if (finished) {
        //            /** Flush the encoder as it may have delayed frames. */
        //            do {
        //                if (encode_video_frame(NULL, &data_written))
        //                    goto cleanup;
        //            } while (data_written);
        //            ret = 1;
        //        }
    }

    return ret;
cleanup:
    return -1;
}

/*
         * Get the actual number of bytes in the encoded file, i.e. without any
         * padding. Valid only after encode_finish() has been called.
         */
size_t FfmpegTranscoder::get_actual_size() const {
    return m_nActual_size;
}

/*
         * Properly calculate final file size. This is the sum of the size of
         * ID3v2, ID3v1, and raw MP3 data. This is theoretically only approximate
         * but in practice gives excellent answers, usually exactly correct.
         * Cast to 64-bit int to avoid overflow.
         */
size_t FfmpegTranscoder::calculate_size() const {
    if (m_nActual_size != 0)
    {
        // Do not recalculate over, use cached size
        return m_nActual_size;
    }
    else if (m_in.m_pFormat_ctx != NULL)
    {
        // TODO: das rechent ne Menge Bleedsinn aus, awer nix Gorregdes...
        size_t size = 0;
        AVCodecID audio_codec_id = AV_CODEC_ID_AAC;
        AVCodecID video_codec_id = AV_CODEC_ID_H264;

        if (m_in.m_nAudio_stream_idx > -1)
        {
            switch (audio_codec_id)
            {
            case AV_CODEC_ID_AAC:
                // TODO: calculate correct size of mp3
                size += (size_t)(ffmpeg_cvttime(m_in.m_pFormat_ctx->duration, AV_TIME_BASE_Q) * params.audiobitrate / 8);
                break;
            case AV_CODEC_ID_MP3:
                // TODO: calculate correct size of mp3
                size += (size_t)(ffmpeg_cvttime(m_in.m_pFormat_ctx->duration, AV_TIME_BASE_Q) * params.audiobitrate / 8);
                break;
            default:
                mp3fs_error("FFMPEG transcoder: Internal error - unsupported audio codec %s.", get_codec_name(audio_codec_id));
                break;
            }
        }

        if (m_in.m_nVideo_stream_idx > -1)
        {
            if (m_bIsVideo)
            {
                switch (video_codec_id)
                {
                case AV_CODEC_ID_H264:
                case AV_CODEC_ID_MJPEG:
                    size += (size_t)(ffmpeg_cvttime(m_in.m_pFormat_ctx->duration, AV_TIME_BASE_Q) * params.videobitrate);
                    break;
                default:
                    mp3fs_error("FFMPEG transcoder: Internal error - unsupported video codec %s.", get_codec_name(video_codec_id));
                    break;
                }
            }
            //            else      TODO: Add picture size
            //            {

            //            }
        }

        size *= 1250;   // TODO: Magic number

        return size;
    }
    else
    {
        // Unknown...
        return 0;
    }
}

/*
         * Encode any remaining PCM data in LAME internal buffers to the given
         * Buffer. This should be called after all input data has already been
         * passed to encode_pcm_data().
         */
int FfmpegTranscoder::encode_finish(Buffer& buffer) {

    int ret = 0;

    /** Write the trailer of the output file container. */
    ret = write_output_file_trailer();
    if (ret < 0)
    {
        mp3fs_error("FFMPEG transcoder: Error writing trailer (error '%s').", ffmpeg_geterror(ret).c_str());
        ret = -1;
    }

    m_nActual_size = buffer.actual_size();

    return 1;
}

const ID3v1 * FfmpegTranscoder::id3v1tag() const
{
    return &m_out.m_id3v1;
}

int FfmpegTranscoder::writePacket(void * pOpaque, unsigned char * pBuffer, int nBufSize)
{
    Buffer * buffer = (Buffer *)pOpaque;

    if (buffer == NULL)
    {
        return -1;
    }

    return (int)buffer->write((const uint8_t*)pBuffer, nBufSize);
}

int64_t FfmpegTranscoder::seek(void * pOpaque, int64_t i4Offset, int nWhence)
{
    Buffer * buffer = (Buffer *)pOpaque;
    int64_t i64ResOffset = 0;

    if (buffer != NULL)
    {
        if (nWhence & AVSEEK_SIZE)
        {
            i64ResOffset = buffer->tell();
        }

        else
        {
            nWhence &= ~(AVSEEK_SIZE | AVSEEK_FORCE);

            switch (nWhence)
            {
            case SEEK_CUR:
            {
                i4Offset = buffer->tell() + i4Offset;
                break;
            }

            case SEEK_END:
            {
                i4Offset = buffer->size() - i4Offset;
                break;
            }

            case SEEK_SET:  // SEEK_SET only supported
            {
                break;
            }
            }

            if (i4Offset < 0)
            {
                i4Offset = 0;
            }

            if (buffer->seek(i4Offset))
            {
                i64ResOffset = i4Offset;
            }

            else
            {
                i64ResOffset = 0;
            }
        }
    }

    return i64ResOffset;
}

#pragma GCC diagnostic pop