// Copyright (c) 2024 AgiBot D1 Project
// RTSP video source implementation

#include "rtsp_source.h"
#include <stdio.h>
#include <string.h>
#include <gst/allocators/gstdmabuf.h>

int rtsp_source_init(rtsp_source_t *src, const char *rtsp_url) {
    gst_init(NULL, NULL);

    src->rtsp_url = rtsp_url;
    src->running = false;

    // 设置 GST_PLUGIN_PATH（板端需要）
    setenv("GST_PLUGIN_PATH", "/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0", 1);

    // 构建 pipeline：RTSP → VPU 硬解 → NV12
    char pipeline_str[1024];
    snprintf(pipeline_str, sizeof(pipeline_str),
             "rtspsrc location=%s protocols=tcp latency=0 ! "
             "rtph264depay ! h264parse ! "
             "mppvideodec ! "
             "video/x-raw,format=NV12 ! "
             "appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true",
             rtsp_url);

    GError *error = NULL;
    src->pipeline = gst_parse_launch(pipeline_str, &error);
    if (!src->pipeline) {
        printf("Failed to create pipeline: %s\n", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return -1;
    }

    src->appsink = gst_bin_get_by_name(GST_BIN(src->pipeline), "sink");
    if (!src->appsink) {
        printf("Failed to get appsink\n");
        gst_object_unref(src->pipeline);
        return -1;
    }

    // 启动 pipeline
    GstStateChangeReturn state_ret = gst_element_set_state(src->pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        printf("Failed to start pipeline\n");
        gst_object_unref(src->appsink);
        gst_object_unref(src->pipeline);
        return -1;
    }

    src->running = true;
    printf("RTSP source initialized: %s\n", rtsp_url);
    return 0;
}

int rtsp_source_get_frame(rtsp_source_t *src, image_buffer_t *img) {
    if (!src->running) {
        return -1;
    }

    // 从 appsink 拉取一帧
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(src->appsink));
    if (!sample) {
        printf("Failed to pull sample from appsink\n");
        return -1;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstStructure *structure = gst_caps_get_structure(caps, 0);

    // 获取分辨率
    int width = 0, height = 0;
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);

    img->width = width;
    img->height = height;
    img->format = IMAGE_FORMAT_YUV420SP_NV12;

    // 简化方案：直接拷贝整个 buffer（暂时不处理 stride）
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        printf("Failed to map buffer\n");
        gst_sample_unref(sample);
        return -1;
    }

    // mppvideodec 输出 Y 平面带垂直 padding（实测 map.size=3133440=1920*1088*1.5），
    // UV 平面起点在 width*vstride 处；这里拼成紧凑的 width x height NV12
    int vstride = (int)((size_t)map.size * 2 / 3 / width);
    if (vstride < height) {
        printf("Unexpected buffer size %zu for %dx%d\n", (size_t)map.size, width, height);
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return -1;
    }

    static bool probe_printed = false;
    if (!probe_printed) {
        printf("[Probe] map.size=%zu width=%d height=%d vstride=%d\n",
               (size_t)map.size, width, height, vstride);
        probe_printed = true;
    }

    img->size = width * height * 3 / 2;
    img->fd = 0;

    if (img->virt_addr == NULL) {
        img->virt_addr = (unsigned char*)malloc(img->size);
        if (img->virt_addr == NULL) {
            printf("Failed to allocate memory for image\n");
            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
            return -1;
        }
    }

    memcpy(img->virt_addr, map.data, (size_t)width * height);
    memcpy(img->virt_addr + (size_t)width * height,
           map.data + (size_t)width * vstride,
           (size_t)width * height / 2);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return 0;
}

void rtsp_source_release(rtsp_source_t *src) {
    if (src->running) {
        gst_element_set_state(src->pipeline, GST_STATE_NULL);
        gst_object_unref(src->appsink);
        gst_object_unref(src->pipeline);
        src->running = false;
        printf("RTSP source released\n");
    }
}

// ========== RTSP 推流器实现 ==========

int rtsp_streamer_init(rtsp_streamer_t *streamer, int port, int width, int height) {
    streamer->port = port;
    streamer->running = false;

    // 构建 pipeline：完全对齐 Phase 3 Bash 成功方案
    // 关键：config-interval=1 确保 SPS/PPS 周期性发送
    char pipeline_str[2048];
    snprintf(pipeline_str, sizeof(pipeline_str),
             "appsrc name=src format=time is-live=true do-timestamp=true "
             "caps=video/x-raw,format=NV12,width=%d,height=%d,framerate=30/1 ! "
             "mpph264enc bps=2000000 ! "
             "h264parse config-interval=1 ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! "
             "mpegtsmux ! "
             "tcpserversink host=0.0.0.0 port=%d sync=false",
             width, height, port);

    GError *error = NULL;
    streamer->pipeline = gst_parse_launch(pipeline_str, &error);
    if (!streamer->pipeline) {
        printf("Failed to create streamer pipeline: %s\n", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return -1;
    }

    streamer->appsrc = gst_bin_get_by_name(GST_BIN(streamer->pipeline), "src");
    if (!streamer->appsrc) {
        printf("Failed to get appsrc\n");
        gst_object_unref(streamer->pipeline);
        return -1;
    }

    // 启动 pipeline
    GstStateChangeReturn state_ret = gst_element_set_state(streamer->pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        printf("Failed to start streamer pipeline\n");
        gst_object_unref(streamer->appsrc);
        gst_object_unref(streamer->pipeline);
        return -1;
    }

    streamer->running = true;
    printf("RTSP streamer initialized on port %d\n", port);
    return 0;
}

int rtsp_streamer_push_frame(rtsp_streamer_t *streamer, image_buffer_t *img) {
    if (!streamer->running) {
        return -1;
    }

    // 创建 GstBuffer
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, img->size, NULL);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        return -1;
    }

    // 填充数据
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        printf("Failed to map buffer for writing\n");
        gst_buffer_unref(buffer);
        return -1;
    }

    memcpy(map.data, img->virt_addr, img->size);
    gst_buffer_unmap(buffer, &map);

    // 推送到 appsrc
    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(streamer->appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        printf("Failed to push buffer to appsrc: %d\n", ret);
        return -1;
    }

    return 0;
}

void rtsp_streamer_release(rtsp_streamer_t *streamer) {
    if (streamer->running) {
        gst_app_src_end_of_stream(GST_APP_SRC(streamer->appsrc));
        gst_element_set_state(streamer->pipeline, GST_STATE_NULL);
        gst_object_unref(streamer->appsrc);
        gst_object_unref(streamer->pipeline);
        streamer->running = false;
        printf("RTSP streamer released\n");
    }
}
