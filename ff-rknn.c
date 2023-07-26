/*
 * ff-rknn - Decode H264, ai inference, render it on screen
 * Alexander Finger <alex.mobigo@gmail.com>
 *
 * This code has been tested on Rockchip RK3588 platform
 *      kernel v5.10.110 BSP
 *      ffmpeg 4.4.2 / ffmpeg 5.1 / ffmpeg 6 + SDL3 + RKNN support
 *
 * FFMPEG DRM/KMS example application
 * Jorge Ramirez-Ortiz <jramirez@baylibre.com>
 *
 * Main file of the application
 *      Based on code from:
 *              2001 Fabrice Bellard (FFMPEG/doc/examples/decode_video.codec_ctx
 *              2018 Stanimir Varbanov (v4l2-decode/src/drm.codec_ctx)
 *
 * This code has been tested on Linaro's Dragonboard 820c
 *      kernel v4.14.15, venus decoder
 *      ffmpeg 4.0 + lrusacks ffmpeg/DRM support + review
 *              https://github.com/ldts/ffmpeg  branch lrusak/v4l2-drmprime
 *
 *
 * Copyright (codec_ctx) 2018 Baylibre
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "SDL3/SDL.h"
#include "SDL_syswm.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>

#include <rga/RgaApi.h>
#include <rga/rga.h>

#ifdef __cplusplus
} // closing brace for extern "C"
#endif

#include "postprocess.h"
#include "rknn_api.h"

#define ALIGN(x, a)           ((x) + (a - 1)) & (~(a - 1))
#define DRM_ALIGN(val, align) ((val + (align - 1)) & ~(align - 1))

#ifndef DRM_FORMAT_NV12_10
#define DRM_FORMAT_NV12_10 fourcc_code('N', 'A', '1', '2')
#endif

#ifndef DRM_FORMAT_NV15
#define DRM_FORMAT_NV15 fourcc_code('N', 'A', '1', '5')
#endif

#define MODEL_WIDTH  640
#define MODEL_HEIGHT 640

#define arg_i 36438 // -i
#define arg_x 36453 // -x
#define arg_y 36454 // -y
#define arg_l 36441 // -l
#define arg_m 36442 // -m
#define arg_t 36449 // -t
#define arg_f 36435 // -f
#define arg_r 36447 // -r
#define arg_d 36433 // -d
#define arg_p 36445 // -p
#define arg_s 36448 // -s

/* --- RKNN --- */
int channel = 3;
int width = MODEL_WIDTH;
int height = MODEL_HEIGHT;
unsigned char *model_data;
int model_data_size = 0;
char *model_name = NULL;
float scale_w = 1.0f; // (float)width / img_width;
float scale_h = 1.0f; // (float)height / img_height;
detect_result_group_t detect_result_group;
std::vector<float> out_scales;
std::vector<int32_t> out_zps;
rknn_context ctx;
rknn_input_output_num io_num;
rknn_input inputs[1];
rknn_tensor_attr output_attrs[128];
size_t actual_size = 0;
const float nms_threshold = NMS_THRESH;
const float box_conf_threshold = BOX_THRESH;
/* --- SDL --- */
int frameSize_texture;
int frameSize_rknn;
void *resize_buf;
void *texture_dst_buf;
Uint32 format;
SDL_Texture *texture;
SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;

int screen_width = 960;
int screen_height = 540;
int screen_left = 0;
int screen_top = 0;
unsigned int frame_width = 960;
unsigned int frame_height = 540;
int v4l2;  // v4l2 h264
int rtsp;  // rtsp h264
int rtmp;  // flv h264
int http;  // flv h264
int delay; // ms
char *pixel_format;
char *sensor_frame_size;
char *sensor_frame_rate;

float frmrate = 0.0;      // Measured frame rate
float avg_frmrate = 0.0;  // avg frame rate
float prev_frmrate = 0.0; // avg frame rate
Uint32 currtime;
Uint32 lasttime;
int loop_counter = 0;
const int frmrate_update = 25;

enum AVPixelFormat get_format(AVCodecContext *Context,
                              const enum AVPixelFormat *PixFmt)
{
    while (*PixFmt != AV_PIX_FMT_NONE) {
        if (*PixFmt == AV_PIX_FMT_DRM_PRIME)
            return AV_PIX_FMT_DRM_PRIME;
        PixFmt++;
    }
    return AV_PIX_FMT_NONE;
}

static int drm_rga_buf(int src_Width, int src_Height, int src_fd,
                       int src_format, int dst_Width, int dst_Height,
                       int dst_format, int frameSize, char *buf)
{
    rga_info_t src;
    rga_info_t dst;
    int ret;
    int hStride = (src_Height + 15) & (~15);
    int wStride = (src_Width + 15) & (~15);
    // int dhStride = (dst_Height + 15) & (~15);
    // int dwStride = (dst_Width + 15) & (~15);

    memset(&src, 0, sizeof(rga_info_t));
    src.fd = src_fd;
    src.mmuFlag = 1;

    memset(&dst, 0, sizeof(rga_info_t));
    dst.fd = -1;
    dst.virAddr = buf;
    dst.mmuFlag = 1;

    rga_set_rect(&src.rect, 0, 0, src_Width, src_Height, wStride, hStride,
                 src_format);
    rga_set_rect(&dst.rect, 0, 0, dst_Width, dst_Height, dst_Width, dst_Height,
                 dst_format);

    ret = c_RkRgaBlit(&src, &dst, NULL);
    return ret;
}

#if 0
static char *drm_get_rgaformat_str(uint32_t drm_fmt)
{
  switch (drm_fmt) {
  case DRM_FORMAT_NV12:
    return "RK_FORMAT_YCbCr_420_SP";
  case DRM_FORMAT_NV12_10:
    return "RK_FORMAT_YCbCr_420_SP_10B";
  case DRM_FORMAT_NV15:
    return "RK_FORMAT_YCbCr_420_SP_10B";
  case DRM_FORMAT_NV16:
    return "RK_FORMAT_YCbCr_422_SP";
  case DRM_FORMAT_YUYV:
    return "RK_FORMAT_YUYV_422";
  case DRM_FORMAT_UYVY:
    return "RK_FORMAT_UYVY_422";
  default:
    return "0";
  }
}
#endif

static uint32_t drm_get_rgaformat(uint32_t drm_fmt)
{
    switch (drm_fmt) {
    case DRM_FORMAT_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case DRM_FORMAT_NV12_10:
        return RK_FORMAT_YCbCr_420_SP_10B;
    case DRM_FORMAT_NV15:
        return RK_FORMAT_YCbCr_420_SP_10B;
    case DRM_FORMAT_NV16:
        return RK_FORMAT_YCbCr_422_SP;
    case DRM_FORMAT_YUYV:
        return RK_FORMAT_YUYV_422;
    case DRM_FORMAT_UYVY:
        return RK_FORMAT_UYVY_422;
    default:
        return 0;
    }
}

static void displayTexture(void *imageData)
{
    unsigned char *texture_data = NULL;
    int texture_pitch = 0;

    if (loop_counter++ % frmrate_update == 0) {
        currtime = SDL_GetTicks(); // [ms]
        if (currtime - lasttime > 0) {
            frmrate = frmrate_update * (1000.0 / (currtime - lasttime));
        }
        lasttime = currtime;
    }

    SDL_LockTexture(texture, 0, (void **)&texture_data, &texture_pitch);
    memcpy(texture_data, (void *)imageData, frameSize_texture);
    SDL_UnlockTexture(texture);
    SDL_RenderTexture(renderer, texture, NULL, NULL);

    // Draw Objects
    char text[256];
    SDL_FRect rect;
    for (int i = 0; i < detect_result_group.count; i++) {
        detect_result_t *det_result = &(detect_result_group.results[i]);
#if 0
    sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
    printf("%s @ (%d %d %d %d) %f\n",
           det_result->name,
           det_result->box.left,
           det_result->box.top,
           det_result->box.right,
           det_result->box.bottom,
           det_result->prop);
#endif
        if (det_result->name[0] == 'p' && det_result->name[1] == 'e')
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'c' && det_result->name[1] == 'a')
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'b' && det_result->name[1] == 'u')
            SDL_SetRenderDrawColor(renderer, 255, 0, 255, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'b' && det_result->name[1] == 'i')
            SDL_SetRenderDrawColor(renderer, 255, 255, 0, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'm' && det_result->name[1] == 'o')
            SDL_SetRenderDrawColor(renderer, 128, 155, 255, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'b' && det_result->name[3] == 'k')
            SDL_SetRenderDrawColor(renderer, 128, 128, 128, SDL_ALPHA_OPAQUE);
        else if (det_result->name[0] == 'u' && det_result->name[1] == 'm')
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
        else
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, SDL_ALPHA_OPAQUE);

        rect.x = det_result->box.left;
        rect.y = det_result->box.top;
        rect.w = det_result->box.right - det_result->box.left;
        rect.h = det_result->box.bottom - det_result->box.top;
        SDL_RenderRect(renderer, &rect);
    }

    SDL_RenderPresent(renderer);

    avg_frmrate = (prev_frmrate + frmrate) / 2.0;
    prev_frmrate = frmrate;
}

static int decode_and_display(AVCodecContext *dec_ctx, AVFrame *frame,
                              AVPacket *pkt)
{
    AVDRMFrameDescriptor *desc;
    AVDRMLayerDescriptor *layer;
    unsigned int drm_format;
    RgaSURF_FORMAT src_format;
    RgaSURF_FORMAT dst_format;
    SDL_Rect rect;
    int ret;

    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        return ret;
    }
    ret = 0;
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error during decoding!\n");
            return ret;
        }
        desc = (AVDRMFrameDescriptor *)frame->data[0];
        layer = &desc->layers[0];
        if (desc && layer) {
            drm_format = layer->format;
            src_format = (RgaSURF_FORMAT)drm_get_rgaformat(drm_format);

            /* ------------ RKNN ----------- */
            drm_rga_buf(frame->width, frame->height, desc->objects[0].fd, src_format,
                        screen_width, screen_height, RK_FORMAT_RGB_888,
                        frameSize_texture, (char *)texture_dst_buf);
            drm_rga_buf(frame->width, frame->height, desc->objects[0].fd, src_format,
                        width, height, RK_FORMAT_RGB_888, frameSize_rknn, (char *)resize_buf);

            inputs[0].buf = resize_buf;
            rknn_inputs_set(ctx, io_num.n_input, inputs);

            rknn_output outputs[io_num.n_output];
            memset(outputs, 0, sizeof(outputs));
            for (int i = 0; i < io_num.n_output; i++) {
                outputs[i].want_float = 0;
            }

            ret = rknn_run(ctx, NULL);
            ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);

            // post process
            scale_w = (float)width / screen_width;
            scale_h = (float)height / screen_height;

            for (int i = 0; i < io_num.n_output; ++i) {
                out_scales.push_back(output_attrs[i].scale);
                out_zps.push_back(output_attrs[i].zp);
            }
            post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf,
                         height, width, box_conf_threshold, nms_threshold,
                         scale_w, scale_h, out_zps, out_scales, &detect_result_group);

            displayTexture(texture_dst_buf);
            ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
        }
    }
    return 0;
}

static unsigned int hash_me(char *str)
{
    unsigned int hash = 32;
    while (*str) {
        hash = ((hash << 5) + hash) + (*str++);
    }
    return hash;
}

void print_help(void)
{
    fprintf(stderr, "ff-rknn parameters:\n"
                    "-x displayed width\n"
                    "-y displayed height\n"
                    "-m rknn model\n"
                    "-f protocol (v4l2, rtsp, rtmp, http)\n"
                    "-p pixel format (h264) - camera\n"
                    "-s video frame size (WxH) - camera\n"
                    "-r video frame rate - camera\n");
}

/*-------------------------------------------
  Functions
  -------------------------------------------*/
static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp) {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0) {
        fprintf(stderr, "blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL) {
        fprintf(stderr, "buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(char *filename, int *model_size)
{

    FILE *fp;
    unsigned char *data;

    if (!filename)
        return NULL;

    fp = fopen(filename, "rb");
    if (NULL == fp) {
        fprintf(stderr, "Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

static int saveFloat(const char *file_name, float *output, int element_size)
{
    FILE *fp;
    fp = fopen(file_name, "w");
    for (int i = 0; i < element_size; i++) {
        fprintf(fp, "%.6f\n", output[i]);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[])
{
    SDL_Event event;
    SDL_SysWMinfo info;
    SDL_version sdl_compiled;
    SDL_version sdl_linked;
    Uint32 wflags = 0 | SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS |
                    SDL_WINDOW_ALWAYS_ON_TOP;
    AVFormatContext *input_ctx = NULL;
    AVStream *video = NULL;
    int video_stream, ret, v4l2 = 0, kmsgrab = 0;
    AVCodecContext *codec_ctx = NULL;
    AVCodec *codec;
    AVFrame *frame;
    AVPacket pkt;
    int lindex, opt;
    char *codec_name = NULL;
    char *video_name = NULL;
    // char *video_name = "/home/rock/weston/apps/videos_rknn/vid-1.mp4";
    // char *video_name = "/home/rock/Videos/jellyfish-5-mbps-hd-hevc.mkv";
    char *pixel_format = NULL, *size_window = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *dict = NULL;
    AVCodecParameters *codecpar;
    AVInputFormat *ifmt = NULL;
    int nframe = 1;
    int finished = 0;
    int i = 1;
    unsigned int a;

    a = 0;

    while (i < argc) {
        a = hash_me(argv[i++]);
        switch (a) {
        case arg_i:
            video_name = argv[i];
            break;
        case arg_x:
            screen_width = atoi(argv[i]);
            break;
        case arg_y:
            screen_height = atoi(argv[i]);
            break;
        case arg_l:
            screen_left = atoi(argv[i]);
            break;
        case arg_t:
            screen_top = atoi(argv[i]);
            break;
        case arg_f:
            // v4l2 = atoi(argv[i]);
            v4l2 = !strncasecmp(argv[i], "v4l2", 4);
            rtsp = !strncasecmp(argv[i], "rtsp", 4);
            rtmp = !strncasecmp(argv[i], "rtmp", 4);
            http = !strncasecmp(argv[i], "http", 4);
            break;
        case arg_r:
            sensor_frame_rate = argv[i];
            break;
        case arg_d:
            delay = atoi(argv[i]);
            break;
        case arg_p:
            pixel_format = argv[i];
            break;
        case arg_s:
            sensor_frame_size = argv[i];
            break;
        case arg_m:
            model_name = argv[i];
            break;
        default:
            break;
        }
        i++;
    }
    // fprintf(stderr,"%s: %u\n", "-p", hash_me("-p"));
    // fprintf(stderr,"%s: %u\n", "-s", hash_me("-s"));

    if (!video_name) {
        fprintf(stderr, "No stream to play! Please pass an input.\n");
        print_help();
        return -1;
    }
    if (!model_name) {
        fprintf(stderr, "No model to load! Please pass a model.\n");
        print_help();
        return -1;
    }
    if (screen_width <= 0)
        screen_width = 960;
    if (screen_height <= 0)
        screen_height = 540;
    if (screen_left <= 0)
        screen_left = 0;
    if (screen_top <= 0)
        screen_top = 0;

    /* Create the neural network */
    model_data_size = 0;
    model_data = load_model(model_name, &model_data_size);
    if (!model_data) {
        fprintf(stderr, "Error locading model: `%s`\n", model_name);
        return -1;
    }
    fprintf(stderr, "Model: %s - size: %d.\n", model_name, model_data_size);
    ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "rknn_init error ret=%d\n", ret);
        return -1;
    }

    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version,
                     sizeof(rknn_sdk_version));
    if (ret < 0) {
        fprintf(stderr, "rknn_init error ret=%d\n", ret);
        return -1;
    }
    fprintf(stderr, "sdk version: %s driver version: %s\n",
            version.api_version,
            version.drv_version);

    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        fprintf(stderr, "rknn_init error ret=%d\n", ret);
        return -1;
    }
    fprintf(stderr, "model input num: %d, output num: %d\n",
            io_num.n_input,
            io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input + 1];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        // fprintf(stderr, "RKNN_QUERY_OUTPUT_ATTR output_attrs[%d].index=%d\n", i, i);
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret < 0) {
            fprintf(stderr, "rknn_init error ret=%d\n", ret);
            return -1;
        }
    }

    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]),
                         sizeof(rknn_tensor_attr));
    }

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        channel = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        height = input_attrs[0].dims[3];
    } else {
        width = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }

    fprintf(stderr, "model: %dx%dx%d\n", width, height, channel);
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = width * height * channel;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0;

    input_ctx = avformat_alloc_context();
    if (!input_ctx) {
        av_log(0, AV_LOG_ERROR, "Cannot allocate input format (Out of memory?)\n");
        return -1;
    }

    av_dict_set(&opts, "num_capture_buffers", "128", 0);
    if (rtsp) {
        // av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "rtsp_flags", "prefer_tcp", 0);
    }
    if (v4l2) {
        avdevice_register_all();
        ifmt = av_find_input_format("video4linux2");
        if (!ifmt) {
            av_log(0, AV_LOG_ERROR, "Cannot find input format: v4l2\n");
            return -1;
        }
        input_ctx->flags |= AVFMT_FLAG_NONBLOCK;
        if (pixel_format) {
            av_dict_set(&opts, "input_format", pixel_format, 0);
        }
        if (sensor_frame_size)
            av_dict_set(&opts, "video_size", sensor_frame_size, 0);
        if (sensor_frame_rate)
            av_dict_set(&opts, "framerate", sensor_frame_rate, 0);
    }
    if (rtmp) {
        ifmt = av_find_input_format("flv");
        if (!ifmt) {
            av_log(0, AV_LOG_ERROR, "Cannot find input format: flv\n");
            return -1;
        }
        av_dict_set(&opts, "fflags", "nobuffer", 0);
    }

    if (http) {
        av_dict_set(&opts, "fflags", "nobuffer", 0);
    }

    if (avformat_open_input(&input_ctx, video_name, ifmt, &opts) != 0) {
        av_log(0, AV_LOG_ERROR, "Cannot open input file '%s'\n", video_name);
        avformat_close_input(&input_ctx);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        av_log(0, AV_LOG_ERROR, "Cannot find input stream information.\n");
        avformat_close_input(&input_ctx);
        return -1;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ret < 0) {
        av_log(0, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        avformat_close_input(&input_ctx);
        return -1;
    }
    video_stream = ret;

    /* find the video decoder: ie: h264_rkmpp / h264_rkmpp_decoder */
    codecpar = input_ctx->streams[video_stream]->codecpar;
    if (!codecpar) {
        av_log(0, AV_LOG_ERROR, "Unable to find stream!\n");
        avformat_close_input(&input_ctx);
        return -1;
    }

    if (codecpar->codec_id != AV_CODEC_ID_H264) {
        av_log(0, AV_LOG_ERROR, "H264 support only!\n");
        avformat_close_input(&input_ctx);
        return -1;
    }

    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        av_log(0, AV_LOG_ERROR, "Codec not found!\n");
        avformat_close_input(&input_ctx);
        return -1;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        av_log(0, AV_LOG_ERROR, "Could not allocate video codec context!\n");
        avformat_close_input(&input_ctx);
        return -1;
    }
    fprintf(stderr, "1 - dict\n");

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(codec_ctx, video->codecpar) < 0) {
        av_log(0, AV_LOG_ERROR, "Error with the codec!\n");
        avformat_close_input(&input_ctx);
        avcodec_free_context(&codec_ctx);
        return -1;
    }

    codec_ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    codec_ctx->coded_height = frame_height;
    codec_ctx->coded_width = frame_width;
    codec_ctx->get_format = get_format;

#if 0
    while (dict = av_dict_get(opts, "", dict, AV_DICT_IGNORE_SUFFIX)) {
        fprintf(stderr, "dict: %s -> %s\n", dict->key, dict->value);
    }
#endif

    /* open it */
    if (avcodec_open2(codec_ctx, codec, &opts) < 0) {
        av_log(0, AV_LOG_ERROR, "Could not open codec!\n");
        avformat_close_input(&input_ctx);
        avcodec_free_context(&codec_ctx);
        return -1;
    }

    av_dict_free(&opts);

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        avformat_close_input(&input_ctx);
        avcodec_free_context(&codec_ctx);
        return -1;
    }

    SDL_VERSION(&sdl_compiled);
    SDL_GetVersion(&sdl_linked);
    SDL_Log("SDL: compiled with=%d.%d.%d linked against=%d.%d.%d",
            sdl_compiled.major, sdl_compiled.minor, sdl_compiled.patch,
            sdl_linked.major, sdl_linked.minor, sdl_linked.patch);

    // SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    // SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_ALLOW_LIBDECOR, "0");
    if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {
        SDL_Log("SDL_Init failed (%s)", SDL_GetError());
        avformat_close_input(&input_ctx);
        avcodec_free_context(&codec_ctx);
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

    if (SDL_CreateWindowAndRenderer(screen_width, screen_height,
                                    wflags,
                                    &window, &renderer) < 0) {
        SDL_Log("SDL_CreateWindowAndRenderer failed (%s)", SDL_GetError());
        goto error_exit;
    }
    // SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    // SDL_RenderFillRect(renderer, &rect);
    SDL_SetWindowTitle(window, "rknn yolov5 object detection");
    SDL_SetWindowPosition(window, screen_left, screen_top);

    format = SDL_PIXELFORMAT_RGB24;
    texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING,
                                screen_width, screen_height);
    if (!texture) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create texturer: %s", SDL_GetError());
        goto error_exit;
    }

    frameSize_rknn = width * height * channel;
    resize_buf = calloc(1, frameSize_rknn + 8 * 1024);

    frameSize_texture = screen_width * screen_height * channel;
    texture_dst_buf = calloc(1, frameSize_texture + 8 * 1024);

    if (!resize_buf || !texture_dst_buf) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create texture buf: %dx%d",
               screen_width, screen_height);
        goto error_exit;
    }

    ret = 0;
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, &pkt)) < 0) {
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                continue;
            }
            break;
        }
        if (video_stream == pkt.stream_index && pkt.size > 0) {
            ret = decode_and_display(codec_ctx, frame, &pkt);
            if (delay > 0)
                usleep(delay * 1000);
        }
        av_packet_unref(&pkt);

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            {
                finished = 1;
                SDL_Log("Program quit after %ld ticks", event.quit.timestamp);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            {
                SDL_bool withControl = (SDL_bool) !!(event.key.keysym.mod & SDL_KMOD_CTRL);
                SDL_bool withShift = (SDL_bool) !!(event.key.keysym.mod & SDL_KMOD_SHIFT);
                SDL_bool withAlt = (SDL_bool) !!(event.key.keysym.mod & SDL_KMOD_ALT);

                switch (event.key.keysym.sym) {
                /* Add hotkeys here */
                case SDLK_ESCAPE:
                    finished = 1;
                    break;
                case SDLK_x:
                    finished = 1;
                    break;
                }
            }
            }
        }
        if (finished) {
            break;
        }
    }
    /* flush the codec */
    decode_and_display(codec_ctx, frame, NULL);

error_exit:

    avformat_close_input(&input_ctx);
    avcodec_free_context(&codec_ctx);
    if (frame) {
        av_frame_free(&frame);
    }
    if (texture_dst_buf) {
        free(texture_dst_buf);
    }
    if (resize_buf) {
        free(resize_buf);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();

    // release
    ret = rknn_destroy(ctx);

    if (model_data) {
        free(model_data);
    }

    fprintf(stderr, "Avg FPS: %.1f\n", avg_frmrate);
}
