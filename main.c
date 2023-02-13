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

static void YUV420P_NV21(const AVFrame *frame_out, unsigned char *image_dst)
{
    int shape = frame_out->width * frame_out->height;
    int size = (shape * 3) >> 1;
    unsigned char p[size];
    av_image_copy_to_buffer(p, size, frame_out->data,
                            frame_out->linesize,
                            AV_PIX_FMT_YUV420P,
                            frame_out->width,
                            frame_out->height,
                            1);

    unsigned char *pNV = image_dst + shape;
    unsigned char *pU = p + shape;
    unsigned char *pV = p + shape + (shape >> 2);

    memcpy(image_dst, p, shape);
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

int main(int argc, char **argv)
{
    int ret;
    AVFrame *frame_in;
    AVFrame *frame_out;

    unsigned char *frame_buffer_out;

    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

    // Input NV21
    FILE *fp_in = fopen(argv[1], "rb+");
    if (fp_in == NULL)
    {
        printf("Error open input file.\n");
        return -1;
    }
    int in_width = 1024;
    int in_height = 576;

    // Output NV21
    FILE *fp_out = fopen(argv[2], "wb+");
    if (fp_out == NULL)
    {
        printf("Error open output file.\n");
        return -1;
    }

    const char *filter_descr = "boxblur";

    char args[512];
    AVFilter const *buffersrc = avfilter_get_by_name("buffer");
    AVFilter const *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    AVBufferSinkParams *buffersink_params;

    filter_graph = avfilter_graph_alloc();

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             in_width,
             in_height,
             AV_PIX_FMT_YUV420P,
             1,
             25,
             1,
             1);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0)
    {
        printf("Cannot create buffer source\n");
        return ret;
    }

    /* buffer video sink: to terminate the filter chain. */
    buffersink_params = av_buffersink_params_alloc();
    buffersink_params->pixel_fmts = pix_fmts;
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, buffersink_params, filter_graph);
    av_free(buffersink_params);
    if (ret < 0)
    {
        printf("Cannot create buffer sink\n");
        return ret;
    }

    /* Endpoints for the filter graph. */
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr,
                                        &inputs, &outputs, NULL)) < 0)
        return ret;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;

    frame_in = av_frame_alloc();
    int image_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    unsigned char *frame_buffer = (unsigned char *)av_malloc(image_buffer_size);
    av_image_fill_arrays(frame_in->data, frame_in->linesize, frame_buffer,
                         AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    frame_out = av_frame_alloc();
    frame_buffer_out = (unsigned char *)av_malloc(image_buffer_size);
    av_image_fill_arrays(frame_out->data, frame_out->linesize, frame_buffer_out,
                         AV_PIX_FMT_YUV420P, in_width, in_height, 1);

    frame_in->width = in_width;
    frame_in->height = in_height;
    frame_in->format = AV_PIX_FMT_YUV420P;

    unsigned char *input_frame = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV21, in_width, in_height, 1));
    int output_size = (in_width * in_height * 3) >> 1;
    while (1)
    {
        if (fread(input_frame, 1, output_size, fp_in) != output_size)
        {
            break;
        }

        NV21_YUV420P(input_frame, frame_buffer, in_width, in_height);
        // input Y,U,V
        frame_in->data[0] = frame_buffer;
        frame_in->data[1] = frame_buffer + in_width * in_height;
        frame_in->data[2] = frame_buffer + ((in_width * in_height * 5) >> 2);

        if (av_buffersrc_add_frame(buffersrc_ctx, frame_in) < 0)
        {
            printf("Error while add frame.\n");
            break;
        }

        /* pull filtered pictures from the filtergraph */
        ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
        if (ret < 0)
            break;

        unsigned char dest[output_size];
        YUV420P_NV21(frame_out, dest);
        fwrite(dest, 1, output_size, fp_out);

        printf("Process 1 frame!\n");
        av_frame_unref(frame_out);
    }

    fclose(fp_in);
    fclose(fp_out);

    av_frame_free(&frame_in);
    av_frame_free(&frame_out);
    avfilter_graph_free(&filter_graph);

    return 0;
}