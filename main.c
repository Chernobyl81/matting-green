#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>

static AVFilterContext *buffersrc_ctx_1;
static AVFilterContext *buffersrc_ctx_2;
static AVFilterContext *buffersink_ctx;
static AVFilterGraph *graph;

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

static void create_filter_spec(char *spec, int size, int width, int height)
{
    snprintf(spec, size, "video_size=%dx%d:pix_fmt=%d:time_base=1/25:pixel_aspect=1/1",
             width, height, AV_PIX_FMT_YUV420P);
}

static int initFilters(int bg_width, int bg_height, int video_width, int video_height)
{

    int returnCode;
    char args[512];
    char args2[512];
    AVFilterInOut *gis = NULL;
    AVFilterInOut *gos = NULL;

    graph = avfilter_graph_alloc();
    if (graph == NULL)
    {
        printf("Cannot allocate filter graph.");
        return -1;
    }

    create_filter_spec(args, sizeof(args), bg_width, bg_height);
    const AVFilter *buffersrc_1 = avfilter_get_by_name("buffer");
    avfilter_graph_create_filter(&buffersrc_ctx_1, buffersrc_1, "in_1", args, NULL, graph);

    create_filter_spec(args2, sizeof(args2), video_width, video_height);
    const AVFilter *buffersrc_2 = avfilter_get_by_name("buffer");
    avfilter_graph_create_filter(&buffersrc_ctx_2, buffersrc_2, "in_2", args2, NULL, graph);

    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVBufferSinkParams *bufferSink_params;
    avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, graph);

    // chromakey filter
    const AVFilter *chroma_filer = avfilter_get_by_name("chromakey");
    AVFilterContext *chroma_filer_ctx;
    avfilter_graph_create_filter(&chroma_filer_ctx, chroma_filer, "chout", "0x008c45:0.15:0.1", NULL, graph);

    // overlay filter
    const AVFilter *overlay_filter = avfilter_get_by_name("overlay");
    AVFilterContext *overlay_filter_ctx;
    avfilter_graph_create_filter(&overlay_filter_ctx, overlay_filter, "overlay", NULL, NULL, graph);

    // link filters to graph
    avfilter_link(buffersrc_ctx_2, 0, chroma_filer_ctx, 0);
    avfilter_link(chroma_filer_ctx, 0, overlay_filter_ctx, 1);
    avfilter_link(buffersrc_ctx_1, 0, overlay_filter_ctx, 0);
    avfilter_link(overlay_filter_ctx, 0, buffersink_ctx, 0);

    returnCode = avfilter_graph_config(graph, NULL);
    if (returnCode < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure graph %d\n", returnCode);
        return returnCode;
    }

    av_log(NULL, AV_LOG_DEBUG, "Graph %s\n", avfilter_graph_dump(graph, NULL));

    return 0;
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 4)
    {
        av_log(NULL, AV_LOG_ERROR, "Usage: <background yuv> <NV21 source file> <NV21 output file>\n ");
        exit(EXIT_FAILURE);
    }

    time_t start_time, finish_time;
   
    int video_width = 1024;
    int video_height = 576;

    // Open background yuv file
    FILE *fp_in = fopen(argv[1], "rb+");
    if (fp_in == NULL)
    {
        printf("Error open input file.\n");
        return -1;
    }

    int bg_width = 1024;
    int bg_height = 576;
    int bg_size = bg_width * bg_height;

    AVFrame *bg_frame = NULL;
    bg_frame = av_frame_alloc();

    int bg_frame_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, bg_width, bg_height, 1);
    unsigned char *bg_frame_buffer = (unsigned char *)av_malloc(bg_frame_buffer_size);
    av_image_fill_arrays(bg_frame->data, bg_frame->linesize, bg_frame_buffer,
                         AV_PIX_FMT_YUV420P, bg_width, bg_height, 1);

    bg_frame->width = bg_width;
    bg_frame->height = bg_height;
    bg_frame->format = AV_PIX_FMT_YUV420P;

    fread(bg_frame_buffer, 1, bg_size * 3 / 2, fp_in);
    bg_frame->data[0] = bg_frame_buffer;
    bg_frame->data[1] = bg_frame_buffer + bg_size;
    bg_frame->data[2] = bg_frame_buffer + ((bg_size * 5) >> 2);

    initFilters(bg_width, bg_height, video_width, video_height);

    int ret;
    AVFrame *video_frame;
    AVFrame *frame_out;
    unsigned char *frame_buffer_out;

    // Input NV21
    FILE *fp_in_nv21 = fopen(argv[2], "rb+");
    if (fp_in_nv21 == NULL)
    {
        printf("Error open input file.\n");
        return -1;
    }

    // Output NV21
    FILE *fp_out = fopen(argv[3], "wb+");
    if (fp_out == NULL)
    {
        printf("Error open output file.\n");
        return -1;
    }

    video_frame = av_frame_alloc();
    int image_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, video_width, video_height, 1);

    unsigned char *frame_buffer = (unsigned char *)av_malloc(image_buffer_size);
    av_image_fill_arrays(video_frame->data, video_frame->linesize, frame_buffer,
                         AV_PIX_FMT_YUV420P, video_width, video_height, 1);

    frame_out = av_frame_alloc();
    frame_buffer_out = (unsigned char *)av_malloc(image_buffer_size);
    av_image_fill_arrays(frame_out->data, frame_out->linesize, frame_buffer_out,
                         AV_PIX_FMT_YUV420P, video_width, video_height, 1);

    video_frame->width = video_width;
    video_frame->height = video_height;
    video_frame->format = AV_PIX_FMT_YUV420P;

    unsigned char *input_frame = (unsigned char *)av_malloc(
        av_image_get_buffer_size(AV_PIX_FMT_NV21, video_width, video_height, 1));

    int output_size = (video_width * video_height * 3) >> 1;

    int nFrames = 0;
    while (1)
    {
        if (fread(input_frame, 1, output_size, fp_in_nv21) != output_size)
        {
            break;
        }
        
        start_time = time(NULL);
        NV21_YUV420P(input_frame, frame_buffer, video_width, video_height);
        // input Y,U,V
        video_frame->data[0] = frame_buffer;
        video_frame->data[1] = frame_buffer + video_width * video_height;
        video_frame->data[2] = frame_buffer + ((video_width * video_height * 5) >> 2);

        if (av_buffersrc_add_frame(buffersrc_ctx_2, video_frame) < 0)
        {
            printf("Error while add frame.\n");
            break;
        }

        if (av_buffersrc_add_frame(buffersrc_ctx_1, bg_frame) < 0)
        {
            printf("Error while add frame.\n");
            exit(EXIT_FAILURE);
        }
        /* pull filtered pictures from the filtergraph */
        ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
        if (ret < 0)
            break;

        unsigned char dest[output_size];
        YUV420P_NV21(frame_out, dest);
        fwrite(dest, 1, output_size, fp_out);

        av_frame_unref(frame_out);
        finish_time = time(NULL);

        double diff_time = difftime(finish_time, start_time);
        nFrames++;
        if (diff_time >= 1.0)
            av_log(NULL, AV_LOG_DEBUG, "%f ms/frame\n", 1000.0/(double)nFrames);
    }

    fclose(fp_in);
    fclose(fp_in_nv21);
    fclose(fp_out);

    av_frame_free(&bg_frame);
    av_frame_free(&video_frame);
    av_frame_free(&frame_out);
    avfilter_graph_free(&graph);

    return 0;
}