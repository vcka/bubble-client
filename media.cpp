#include "media.h"
#include <cstdio>

#include <opencv2/highgui.hpp>

MediaSession::MediaSession(Session *session) :
    mpSession(session), isRunning(false),
    mCodec(NULL), mCodecCtx(NULL), mAvFrame(NULL),
    mSwsCtx(NULL), mRGBFrame(NULL), mRGBFrameBuffer(NULL)
{
    avcodec_register_all();
}

MediaSession::~MediaSession()
{
    if (mCodecCtx)
    {
        avcodec_close(mCodecCtx);
        av_free(mCodecCtx);
    }
    if (mAvFrame)
    {
        av_free(mAvFrame);
    }

    if (mSwsCtx)
    {
        sws_freeContext(mSwsCtx);
    }
    if (mRGBFrame)
    {
        av_free(mRGBFrame);
    }
    if (mRGBFrameBuffer)
    {
        free(mRGBFrameBuffer);
    }
}

int MediaSession::init()
{
    std::printf("[INFO] Initializing media session...\n");
    mCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!mCodec)
    {
        LOG_ERR("Codec not found");
        return -1;
    }

    mCodecCtx = avcodec_alloc_context3(mCodec);
    mAvFrame = av_frame_alloc();

    if (avcodec_open2(mCodecCtx, mCodec, NULL) < 0)
    {
        LOG_ERR("Failed to open codec");
        goto on_error;
    }

    av_init_packet(&mAvPkt);
    std::printf("[INFO] Initialized media session\n");
    return 0;

on_error:
    avcodec_close(mCodecCtx);
    av_free(mCodecCtx);
    av_free(mAvFrame);
    mCodecCtx = NULL;
    mAvFrame = NULL;
    return -1;
}

int MediaSession::start()
{
    int status;
    PackHead *packhead;

    status = init();
    if (status != 0)
    {
        LOG_ERR("Failed to initialize media session");
        return -1;
    }

    std::printf("[INFO] Receiving media frames\n");
    status = mpSession->receive_packet_to_buffer(tmpRecvBuf, sizeof(tmpRecvBuf));
    if (status != 0)
    {
        LOG_ERR("Failed to receive first media frame");
    }
    packhead = (PackHead *)tmpRecvBuf;
    if (packhead->cPackType == 0x08)
    {
        LOG_ERR("The media server is full. Try again later");
        return -1;
    }

    isRunning = true;

    while (isRunning)
    {
        status = processPacket(tmpRecvBuf);
        if (status != 0)
        {
            LOG_ERR("Failed to process media packet");
            break;
        }

        status = mpSession->receive_packet_to_buffer(tmpRecvBuf, sizeof(tmpRecvBuf));
        if (status != 0)
        {
            LOG_ERR("Failed to receive media packet");
            break;
        }
    }

    return 0;
}

int MediaSession::processPacket(char *packet)
{
    PackHead *packhead;
    uint32_t uiPackLen;
    size_t packsize;
    MediaPackData *media_packhead;
    uint32_t uiMediaPackLen;
    char *framedata;

    packhead = (PackHead *)packet;
    uiPackLen = ntohl(packhead->uiLength);
    packsize = uiPackLen + STRUCT_MEMBER_POS(PackHead, cPackType);

    media_packhead = (MediaPackData *)packhead->pData;
    uiMediaPackLen = ntohl(media_packhead->uiLength);
    if (uiMediaPackLen + PACKHEAD_SIZE + STRUCT_MEMBER_POS(MediaPackData, pData) > packsize)
    {
        LOG_ERR("Frame data is larger than packet size");
        return -1;
    }

#ifdef DEBUG
    std::printf("[INFO] Media packet chl: %d type: %d\n", media_packhead->cId, media_packhead->cMediaType);
#endif
    framedata = media_packhead->pData;
    switch (media_packhead->cMediaType)
    {
    case MT_IDR:
    case MT_PSLICE:
        decodeFrame(framedata, uiMediaPackLen);
        break;
    case MT_AUDIO:
        break;
    default:
        LOG_ERR("Unknown media pack type");
    }

    return 0;
}


int MediaSession::decodeFrame(char *buffer, int size)
{
    int status;
    mAvPkt.size = size;
    mAvPkt.data = (uint8_t *)buffer;

    status = avcodec_send_packet(mCodecCtx, &mAvPkt);
    if (status != 0)
    {
        std::fprintf(stderr, "[ERROR] avcodec_send_packet: %d\n", status);
        return -1;
    }

    while (true)
    {
        status = avcodec_receive_frame(mCodecCtx, mAvFrame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
        {
            break;
        }
        if (status != 0)
        {
            LOG_ERR("Error decoding frame");
            return -1;
        }
#ifdef DEBUG
        std::printf("[INFO] Frame decoded w: %d h: %d!!!\n", mCodecCtx->width, mCodecCtx->height);
#endif
        displayFrame(mAvFrame, mCodecCtx->width, mCodecCtx->height);
    }

    return 0;
}

int MediaSession::allocateConversionCtx(enum AVPixelFormat pix_fmt, int width, int height)
{
    if (mSwsCtx)
    {
        return 0;
    }
    assert(mRGBFrame == NULL && mRGBFrameBuffer == NULL);

    mSwsCtx = sws_getContext(width, height, pix_fmt, width, height,
                             AV_PIX_FMT_BGR24, SWS_BICUBIC, NULL, NULL, NULL);
    if (!mSwsCtx)
    {
        LOG_ERR("Failed to allocate sws context");
        return -1;
    }

    mRGBFrame = av_frame_alloc();
    if (!mRGBFrame)
    {
        LOG_ERR("Failed to allocate RGB frame");
        sws_freeContext(mSwsCtx);
        mSwsCtx = NULL;
        return -1;
    }
    mRGBFrame->width = width;
    mRGBFrame->height = height;
    mRGBFrame->format = AV_PIX_FMT_BGR24;

    int nbytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, width, height, 1);
    mRGBFrameBuffer = (uint8_t *)av_malloc(nbytes);
    if (!mRGBFrameBuffer)
    {
        LOG_ERR("Failed to allocate RGB frame buffer");
        sws_freeContext(mSwsCtx);
        av_free(mRGBFrame);
        mSwsCtx = NULL;
        mRGBFrame = NULL;
        return -1;
    }
    av_image_fill_arrays(mRGBFrame->data, mRGBFrame->linesize, mRGBFrameBuffer,
                         AV_PIX_FMT_BGR24, width, height, 1);
    return 0;
}

int MediaSession::displayFrame(AVFrame *frame, int width, int height)
{
    int status;
    status = allocateConversionCtx(mCodecCtx->pix_fmt, width, height);
    if (status < 0)
    {
        LOG_ERR("Failed to allocate conversion resource");
        return -1;
    }

    status = sws_scale(mSwsCtx, frame->data, frame->linesize, 0, height, mRGBFrame->data, mRGBFrame->linesize);
    if (status < 0)
    {
        LOG_ERR("Failed to scale frame");
        return -1;
    }

    cv::Mat mat(height, width, CV_8UC3,  mRGBFrame->data[0], mRGBFrame->linesize[0]);
    cv::namedWindow("Stream");
    cv::imshow("Stream", mat);
    cv::waitKey(10);

    return 0;
}
