#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>

static AVFormatContext *ifmt_ctx;
typedef struct StreamContext
{
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;

    AVFrame *dec_frame;
} StreamContext;
static StreamContext *stream_ctx;
/**
 * @brief
 *
 * @param image_src
 * @param image_dst
 * @param image_width
 * @param image_height
 */
static void NV21_YUV420P(const unsigned char *image_src, unsigned char *image_dst,
                         int image_width, int image_height)
{
    unsigned char *p = image_dst;
    memcpy(p, image_src, (image_width * image_height * 3) >> 1);

    const unsigned char *pNV = image_src + image_width * image_height;
    unsigned char *pU = p + image_width * image_height;
    unsigned char *pV = p + image_width * image_height + ((image_width * image_height) >> 2);

    for (int i = 0; i < (image_width * image_height) >> 1; i++)
    {
        if ((i % 2) == 0)
            *pV++ = *(pNV + i);
        else
            *pU++ = *(pNV + i);
    }
}

/**
 * @brief
 *
 * @param frame_out
 * @param image_dst
 */
static void YUV420P_NV21(const AVFrame *frame_out, unsigned char *image_dst)
{
    int shape = frame_out->width * frame_out->height;

    unsigned char *pY = frame_out->data[0];
    unsigned char *pU = frame_out->data[1];
    unsigned char *pV = frame_out->data[2];

    memcpy(image_dst, pY, shape);
    unsigned char *pNV = image_dst + shape;
    for (int i = 0; i < (shape >> 1); i++)
    {
        if (i % 2 == 0)
        {
            *pNV++ = *pV++;
        }
        else
        {
            *pNV++ = *pU++;
        }
    }
}

static int open_input_file(const char *filename)
{
    int ret;
    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = av_mallocz_array(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        AVStream *stream = ifmt_ctx->streams[i];
        AVCodec const *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                                       "for stream #%u\n",
                   i);
            return ret;
        }
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);

            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }

        stream_ctx[i].dec_ctx = codec_ctx;
        stream_ctx[i].dec_frame = av_frame_alloc();
        if (!stream_ctx[i].dec_frame)
            return AVERROR(ENOMEM);
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static void decode_jpg(const char *filename)
{
    int ret;

    if (open_input_file(filename) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "can not open file\n");
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        av_log(NULL, AV_LOG_ERROR, "alloc packet error\n");
        goto end;
    }

    if ((ret = av_read_frame(ifmt_ctx, pkt)) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "can not read frame\n");
        goto end;
    }

    unsigned int stream_index = pkt->stream_index;
    StreamContext *stream = &stream_ctx[stream_index];
    avcodec_send_packet(stream->dec_ctx, pkt);
    avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);

end:
    av_packet_free(&pkt);
    for (int i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        av_frame_free(&stream_ctx[i].dec_frame);
    }
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
}

AVFilterContext **inputContexts;
AVFilterContext *outputContext;
AVFilterGraph *graph;

static int initFilters(AVFrame *bgFrame, int inputCount, AVCodecContext **codecContexts)
{
    av_log(NULL, AV_LOG_DEBUG, "background size %dx%d\n", bgFrame->width, bgFrame->height);
    int i;
    int returnCode;
    char filters[1024];
    AVFilterInOut *gis = NULL;
    AVFilterInOut *gos = NULL;

    graph = avfilter_graph_alloc();
    if (graph == NULL)
    {
        printf("Cannot allocate filter graph.");
        return -1;
    }

    // build the filters string here
    snprintf(filters, sizeof(filters),
             "buffer=video_size=1920x1080:pix_fmt=0:time_base=1/25:pixel_aspect=3937/3937[in_1];\
    buffer=video_size=1920x1080:pix_fmt=0:time_base=1/180000:pixel_aspect=0/1[in_2];\
    [in_1][in_2]overlay=0:0[result];[result]buffersink");

    returnCode = avfilter_graph_parse2(graph, filters, &gis, &gos);
    if (returnCode < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot parse graph %d\n", returnCode);
        return returnCode;
    }

    returnCode = avfilter_graph_config(graph, NULL);
    if (returnCode < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure graph %d\n", returnCode);
        return returnCode;
    }

    av_log(NULL, AV_LOG_INFO, "Success init filter with return code %d\n", returnCode);
    // get the filter contexts from the graph here
    inputContexts = graph->filters;
    
    return 0;
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_DEBUG);
    FILE *fp_in = fopen(argv[1], "rb+");
    if (fp_in == NULL)
    {
        printf("Error open input file.\n");
        return -1;
    }

    AVFrame *bg_frame = NULL;
    bg_frame = av_frame_alloc();
    int image_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 1920, 1080, 1);
    unsigned char *frame_buffer = (unsigned char *)av_malloc(image_buffer_size);
    av_image_fill_arrays(bg_frame->data, bg_frame->linesize, frame_buffer,
                         AV_PIX_FMT_YUV420P, 1920, 1080, 1);

    bg_frame->width = 1920;
    bg_frame->height = 1080;
    bg_frame->format = AV_PIX_FMT_YUV420P;

    fread(frame_buffer, 1, 1920 * 1080 * 3 / 2, fp_in);
    bg_frame->data[0] = frame_buffer;
    bg_frame->data[1] = frame_buffer + 1920 * 1080;
    bg_frame->data[2] = frame_buffer + ((1920 * 1080 * 5) >> 2);

    initFilters(bg_frame, 0, NULL);

    if (av_buffersrc_add_frame(inputContexts[0], bg_frame) < 0)
    {
        printf("Error while add frame.\n");
        exit(EXIT_FAILURE);  
    }

    // int ret;
    // AVFrame *frame_in;
    // AVFrame *frame_out;

    // unsigned char *frame_buffer_out;

    // AVFilterContext *buffersink_ctx;
    // AVFilterContext *buffersrc_ctx;
    // AVFilterGraph *filter_graph;

    // // Input NV21
    // FILE *fp_in = fopen(argv[1], "rb+");
    // if (fp_in == NULL)
    // {
    //     printf("Error open input file.\n");
    //     return -1;
    // }
    // int in_width = 1024;
    // int in_height = 576;

    // // Output NV21
    // FILE *fp_out = fopen(argv[2], "wb+");
    // if (fp_out == NULL)
    // {
    //     printf("Error open output file.\n");
    //     return -1;
    // }

    // const char *filter_descr = "boxblur";

    // char args[512];
    // snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
    //          in_width,
    //          in_height,
    //          AV_PIX_FMT_YUV420P,
    //          1,
    //          25,
    //          1,
    //          1);
    // AVFilter const *buffersrc = avfilter_get_by_name("buffer");
    // AVFilter const *buffersink = avfilter_get_by_name("buffersink");
    // AVFilterInOut *outputs = avfilter_inout_alloc();
    // AVFilterInOut *inputs = avfilter_inout_alloc();
    // enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    // AVBufferSinkParams *buffersink_params;

    // filter_graph = avfilter_graph_alloc();

    // /* buffer video source: the decoded frames from the decoder will be inserted here. */

    // ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
    //                                    args, NULL, filter_graph);
    // if (ret < 0)
    // {
    //     printf("Cannot create buffer source\n");
    //     return ret;
    // }

    // /* buffer video sink: to terminate the filter chain. */
    // buffersink_params = av_buffersink_params_alloc();
    // buffersink_params->pixel_fmts = pix_fmts;
    // ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
    //                                    NULL, buffersink_params, filter_graph);
    // av_free(buffersink_params);
    // if (ret < 0)
    // {
    //     printf("Cannot create buffer sink\n");
    //     return ret;
    // }

    // /* Endpoints for the filter graph. */
    // outputs->name = av_strdup("in");
    // outputs->filter_ctx = buffersrc_ctx;
    // outputs->pad_idx = 0;
    // outputs->next = NULL;

    // inputs->name = av_strdup("out");
    // inputs->filter_ctx = buffersink_ctx;
    // inputs->pad_idx = 0;
    // inputs->next = NULL;

    // if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr,
    //                                     &inputs, &outputs, NULL)) < 0)
    //     return ret;

    // if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
    //     return ret;

    // frame_in = av_frame_alloc();
    // int image_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    // unsigned char *frame_buffer = (unsigned char *)av_malloc(image_buffer_size);
    // av_image_fill_arrays(frame_in->data, frame_in->linesize, frame_buffer,
    //                      AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    // frame_out = av_frame_alloc();
    // frame_buffer_out = (unsigned char *)av_malloc(image_buffer_size);
    // av_image_fill_arrays(frame_out->data, frame_out->linesize, frame_buffer_out,
    //                      AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    // frame_in->width = in_width;
    // frame_in->height = in_height;
    // frame_in->format = AV_PIX_FMT_YUV420P;

    // unsigned char *input_frame = (unsigned char *)av_malloc(
    //     av_image_get_buffer_size(AV_PIX_FMT_NV21, in_width, in_height, 1));

    // int output_size = (in_width * in_height * 3) >> 1;
    // while (1)
    // {
    //     if (fread(input_frame, 1, output_size, fp_in) != output_size)
    //     {
    //         break;
    //     }

    //     NV21_YUV420P(input_frame, frame_buffer, in_width, in_height);
    //     // input Y,U,V
    //     frame_in->data[0] = frame_buffer;
    //     frame_in->data[1] = frame_buffer + in_width * in_height;
    //     frame_in->data[2] = frame_buffer + ((in_width * in_height * 5) >> 2);

    //     if (av_buffersrc_add_frame(buffersrc_ctx, frame_in) < 0)
    //     {
    //         printf("Error while add frame.\n");
    //         break;
    //     }

    //     /* pull filtered pictures from the filtergraph */
    //     ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
    //     if (ret < 0)
    //         break;

    //     unsigned char dest[output_size];
    //     YUV420P_NV21(frame_out, dest);
    //     fwrite(dest, 1, output_size, fp_out);

    //     printf("Process 1 frame!\n");
    //     av_frame_unref(frame_out);
    // }

    // fclose(fp_in);
    // fclose(fp_out);

    // av_frame_free(&frame_in);
    // av_frame_free(&frame_out);
    // avfilter_graph_free(&filter_graph);

    return 0;
}