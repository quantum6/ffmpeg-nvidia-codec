#include <stdio.h>
#include <stdlib.h>


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#define LOG_HERE() printf("%s-%d\n", __func__, __LINE__)

#define DUMP_FRAME(frame) { \
   printf( "%s-%d AVFrame:format=%2d, key_frame=%d, pict_type=%d, width=%4d, height=%4d, data=(%d, %d, %d), linesize=(%4d, %4d, %4d)\n", \
    __func__, __LINE__, \
    frame->format, frame->key_frame, frame->pict_type, \
    frame->width,  frame->height, \
   (frame->data[0] != NULL), \
   (frame->data[1] != NULL), \
   (frame->data[2] != NULL),\
    frame->linesize[0], \
    frame->linesize[1], \
    frame->linesize[2] \
    );}


#define NVIDIA_H264_DECODER "h264_cuvid"

#ifdef NVIDIA_H264_DECODER

// NVIDIA DECODER result is NV12, filter to YUV420P
static AVFilterContext* decoder_filter_out = NULL;
static AVFilterContext* decoder_filter_in  = NULL;
static AVFilterGraph*   decoder_graph      = NULL;
static int decoder_width  = 0;
static int decoder_height = 0;
static int decoder_format = AV_PIX_FMT_NONE;


static int configure_decoder_filter_graph(AVFilterGraph *graph, 
     AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret;
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) >= 0)
    {
       ret = avfilter_graph_config(graph, NULL);
    }

    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_decoder_video_filters(AVFilterGraph *graph, const int width, const int height, const int format)
{
    int pix_fmts[2] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    char buffersrc_args[256] = {0};
    AVFilterContext *filt_src = NULL, *filt_out = NULL;
    int ret;

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1200000",
             width, height, format);
    if ((ret = avfilter_graph_create_filter(&filt_src,
        avfilter_get_by_name("buffer"), "ffplay_buffer", buffersrc_args,
        NULL, graph)) < 0)
    {
        goto fail;
    }

    ret = avfilter_graph_create_filter(&filt_out,
          avfilter_get_by_name("buffersink"),
          "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
    {
        goto fail;
    }

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    {
        goto fail;
    }
    if ((ret = configure_decoder_filter_graph(graph, filt_src, filt_out)) < 0)
    {
        goto fail;
    }

    decoder_filter_in  = filt_src;
    decoder_filter_out = filt_out;

fail:
    return ret;
}

#endif // NVIDIA_H264_DECODER


int main(int argc, char* argv[])
{
    AVFormatContext *pFormatCtx = NULL;
    AVCodecContext  *pCodecCtx  = NULL;
    AVCodec  *pCodec  = NULL;
    AVFrame  *pFrame  = NULL;
    AVPacket *pPacket = NULL;
    int ret = -1;
    int frame_cnt = 0;
    const char* filepath = "sample_720p-2.h264";

    //分配空间
    pFormatCtx = avformat_alloc_context();

    //打开文件
    if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
    {
        printf("Couldn't open input file.\n");
        return -1;
    }
    av_dump_format(pFormatCtx, 0, filepath, 0);

    //分配空间
    pCodecCtx = avcodec_alloc_context3(NULL);

#ifdef NVIDIA_H264_DECODER

    pCodec = avcodec_find_decoder_by_name(NVIDIA_H264_DECODER);
    printf("Codec %s found %s\n", NVIDIA_H264_DECODER, (pCodec ? "OK." : "failed!"));

#endif

    if (pCodec == NULL)
    {
#if 1
        pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
#else
        if (avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[0]->codecpar) < 0)
        {
            return -1;
        }
        pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
#endif
        if (pCodec == NULL)
        {
            printf("Couldn't find codec.\n");
            return -1;
        }
         printf("Codec found with name %d(%s)\n", pCodec->id, pCodec->long_name);
    }

    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
    {
         printf("Couldn't open codec.\n");
         return -1;
    }

    pFrame = av_frame_alloc();

    pPacket = (AVPacket *)av_malloc(sizeof(AVPacket));
    while (av_read_frame(pFormatCtx, pPacket) >= 0)
    {
        ret = avcodec_send_packet(pCodecCtx, pPacket);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            av_packet_unref(pPacket);
            return -1;
        }

        //printf("pPacket->size=%d\n", pPacket->size);
        ret = avcodec_receive_frame(pCodecCtx, pFrame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            av_packet_unref(pPacket);
            return -1;
        }

        if (pFrame->data == NULL || pFrame->data[0] == NULL)
        {
            continue;
        }

#ifdef NVIDIA_H264_DECODER
        if (   decoder_width  !=      pFrame->width
            || decoder_height !=      pFrame->height
            || decoder_format != (int)pFrame->format)
        {
            decoder_width  =      pFrame->width;
            decoder_height =      pFrame->height;
            decoder_format = (int)pFrame->format;

            decoder_graph = avfilter_graph_alloc();
            configure_decoder_video_filters(decoder_graph, decoder_width, decoder_height, decoder_format);
        }

        if (pFrame->format != AV_PIX_FMT_YUV420P)
        {
            //printf("pFrame->format=%d, AV_PIX_FMT_NV12=%d, AV_PIX_FMT_YUV420P=%d\n", pFrame->format, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P);
            //DUMP_FRAME(pFrame);
            ret = av_buffersrc_add_frame(decoder_filter_in, pFrame);
            //DUMP_FRAME(pFrame);
            ret = av_buffersink_get_frame_flags(decoder_filter_out, pFrame, 0);
        }

#endif

        //DUMP_FRAME(pFrame);
        printf("Frame count=%4d, CodecCtx=(%d, %d)\n", frame_cnt, pCodecCtx->width, pCodecCtx->height);
        frame_cnt++;
        if (frame_cnt > 10)
        {
            break;
        }
    }

    av_packet_unref(pPacket);
    av_frame_unref(pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}

