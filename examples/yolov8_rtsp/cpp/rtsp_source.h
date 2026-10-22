// Copyright (c) 2024 AgiBot D1 Project
// RTSP video source wrapper using GStreamer

#ifndef _RTSP_SOURCE_H_
#define _RTSP_SOURCE_H_

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include "common.h"

typedef struct {
    GstElement *pipeline;
    GstElement *appsink;
    bool running;
    const char *rtsp_url;
} rtsp_source_t;

typedef struct {
    GstElement *pipeline;
    GstElement *appsrc;
    bool running;
    int port;
} rtsp_streamer_t;

// 初始化 RTSP 源
int rtsp_source_init(rtsp_source_t *src, const char *rtsp_url);

// 获取一帧（阻塞）
int rtsp_source_get_frame(rtsp_source_t *src, image_buffer_t *img);

// 释放资源
void rtsp_source_release(rtsp_source_t *src);

// 初始化 RTSP 推流器
int rtsp_streamer_init(rtsp_streamer_t *streamer, int port, int width, int height);

// 推送一帧（NV12 格式，已画框）
int rtsp_streamer_push_frame(rtsp_streamer_t *streamer, image_buffer_t *img);

// 释放推流器
void rtsp_streamer_release(rtsp_streamer_t *streamer);

#endif // _RTSP_SOURCE_H_
