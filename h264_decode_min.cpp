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

typedef struct h264_codec_context_s
{
    AVFormatContext *pFormatCtx;
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    AVFrame         *pFrame;
    AVPacket        *pPacket;

    // NVIDIA DECODER result is NV12, filter to YUV420P
    AVFilterContext* decoder_filter_out;
    AVFilterContext* decoder_filter_in;
    AVFilterGraph*   decoder_graph;

    int decoder_width;
    int decoder_height;

    //AV_PIX_FMT_NONE
    int decoder_format;
    
} h264_codec_context_t;


static int configure_decoder_filter_graph(AVFilterGraph *graph, 
     AVFilterContext *source_ctx, AVFilterContext* sink_ctx)
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

static int configure_decoder_video_filters(h264_codec_context_t* pContext)
{
    int pix_fmts[2] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    char buffersrc_args[256] = {0};
    AVFilterContext *filt_src = NULL, *filt_out = NULL;
    int ret;

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1200000",
             pContext->decoder_width, pContext->decoder_height, pContext->decoder_format);
    if ((ret = avfilter_graph_create_filter(&filt_src,
        avfilter_get_by_name("buffer"), "ffplay_buffer", buffersrc_args,
        NULL, pContext->decoder_graph)) < 0)
    {
        goto fail;
    }

    ret = avfilter_graph_create_filter(&filt_out,
          avfilter_get_by_name("buffersink"),
          "ffplay_buffersink", NULL, NULL, pContext->decoder_graph);
    if (ret < 0)
    {
        goto fail;
    }

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
    {
        goto fail;
    }
    if ((ret = configure_decoder_filter_graph(pContext->decoder_graph, filt_src, filt_out)) < 0)
    {
        goto fail;
    }

    pContext->decoder_filter_in  = filt_src;
    pContext->decoder_filter_out = filt_out;

fail:
    return ret;
}

#endif // NVIDIA_H264_DECODER


int main(int argc, char* argv[])
{
    h264_codec_context_t h264_context = {0};
    int ret = -1;
    int frame_cnt = 0;
    const char* filepath = "sample_720p.h264";

    h264_context.decoder_format = AV_PIX_FMT_NONE;
    
    //分配空间
    h264_context.pFormatCtx = avformat_alloc_context();

    //打开文件
    if (avformat_open_input(&(h264_context.pFormatCtx), filepath, NULL, NULL) != 0)
    {
        printf("Couldn't open input file.\n");
        return -1;
    }
    av_dump_format(h264_context.pFormatCtx, 0, filepath, 0);

    //分配空间
    h264_context.pCodecCtx = avcodec_alloc_context3(NULL);

#ifdef NVIDIA_H264_DECODER

    h264_context.pCodec = avcodec_find_decoder_by_name(NVIDIA_H264_DECODER);
    printf("Codec %s found %s\n", NVIDIA_H264_DECODER, (h264_context.pCodec ? "OK." : "failed!"));

#endif

    if (h264_context.pCodec == NULL)
    {
#if 1
        h264_context.pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
#else
        if (avcodec_parameters_to_context(h264_context.pCodecCtx, h264_context.pFormatCtx->streams[0]->codecpar) < 0)
        {
            return -1;
        }
        h264_context.pCodec = avcodec_find_decoder(h264_context.pCodecCtx->codec_id);
#endif
        if (h264_context.pCodec == NULL)
        {
            printf("Couldn't find codec.\n");
            return -1;
        }
    }

    printf("Codec found with name %d(%s)\n", h264_context.pCodec->id, h264_context.pCodec->long_name);
    if (avcodec_open2(h264_context.pCodecCtx, h264_context.pCodec, NULL) < 0)
    {
         printf("Couldn't open codec.\n");
         return -1;
    }

    h264_context.pFrame = av_frame_alloc();

    h264_context.pPacket = (AVPacket *)av_malloc(sizeof(AVPacket));
    while (av_read_frame(h264_context.pFormatCtx, h264_context.pPacket) >= 0)
    {
        ret = avcodec_send_packet(h264_context.pCodecCtx, h264_context.pPacket);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            av_packet_unref(h264_context.pPacket);
            return -1;
        }

        //printf("pPacket->size=%d\n", h264_context.pPacket->size);
        ret = avcodec_receive_frame(h264_context.pCodecCtx, h264_context.pFrame);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            av_packet_unref(h264_context.pPacket);
            return -1;
        }

        if (h264_context.pFrame->data == NULL || h264_context.pFrame->data[0] == NULL)
        {
            continue;
        }

#ifdef NVIDIA_H264_DECODER
        if (   h264_context.decoder_width  !=      h264_context.pFrame->width
            || h264_context.decoder_height !=      h264_context.pFrame->height
            || h264_context.decoder_format != (int)h264_context.pFrame->format)
        {
            h264_context.decoder_width  =      h264_context.pFrame->width;
            h264_context.decoder_height =      h264_context.pFrame->height;
            h264_context.decoder_format = (int)h264_context.pFrame->format;

            h264_context.decoder_graph = avfilter_graph_alloc();
            configure_decoder_video_filters(&h264_context);
        }

        if (h264_context.pFrame->format != AV_PIX_FMT_YUV420P)
        {
            //printf("pFrame->format=%d, AV_PIX_FMT_NV12=%d, AV_PIX_FMT_YUV420P=%d\n", pFrame->format, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P);
            //DUMP_FRAME(pFrame);
            ret = av_buffersrc_add_frame(h264_context.decoder_filter_in, h264_context.pFrame);
            //DUMP_FRAME(pFrame);
            ret = av_buffersink_get_frame_flags(h264_context.decoder_filter_out, h264_context.pFrame, 0);
        }

#endif

        //DUMP_FRAME(pFrame);
        printf("Frame count=%4d, CodecCtx=(%d, %d)\n", frame_cnt, h264_context.pCodecCtx->width, h264_context.pCodecCtx->height);
        frame_cnt++;
        if (frame_cnt > 10)
        {
            break;
        }
    }

    av_packet_unref(       h264_context.pPacket);
    av_frame_unref(        h264_context.pFrame);
    avcodec_close(         h264_context.pCodecCtx);
    avformat_close_input(&(h264_context.pFormatCtx));

    return 0;
}

