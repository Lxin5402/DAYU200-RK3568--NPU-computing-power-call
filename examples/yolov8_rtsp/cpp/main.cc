// Copyright (c) 2024 AgiBot D1 Project
// Modified from original Rockchip yolov8 demo
// Main changes: Static image input → RTSP stream input

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "yolov8.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "rtsp_source.h"

static bool g_running = true;

void signal_handler(int sig) {
    printf("\nReceived signal %d, stopping...\n", sig);
    g_running = false;
}

// 直接在 NV12 平面上画矩形框（移植自 Phase 3 Python 方案）
// NV12 格式：前 height 行是 Y 平面（亮度），后 height/2 行是 UV 平面（色度，interleaved U/V）
void draw_box_nv12(unsigned char *nv12_data, int width, int height,
                   int x1, int y1, int x2, int y2, int thickness,
                   unsigned char y_val, unsigned char u_val, unsigned char v_val) {
    // 边界检查
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= width) x2 = width - 1;
    if (y2 >= height) y2 = height - 1;
    if (x1 >= x2 || y1 >= y2) return; // 无效框

    unsigned char *y_plane = nv12_data;
    unsigned char *uv_plane = nv12_data + width * height;

    // 画 Y 平面的水平线（上下边）
    for (int t = 0; t < thickness && (y1 + t) < height; t++) {
        // 上边
        if (y1 + t >= 0) {
            for (int x = x1; x <= x2 && x < width; x++) {
                y_plane[(y1 + t) * width + x] = y_val;
            }
        }
        // 下边
        if ((y2 - t) >= 0 && (y2 - t) < height) {
            for (int x = x1; x <= x2 && x < width; x++) {
                y_plane[(y2 - t) * width + x] = y_val;
            }
        }
    }

    // 画 Y 平面的垂直线（左右边）
    for (int t = 0; t < thickness && (x1 + t) < width; t++) {
        // 左边
        if (x1 + t >= 0) {
            for (int y = y1; y <= y2 && y < height; y++) {
                y_plane[y * width + (x1 + t)] = y_val;
            }
        }
        // 右边
        if ((x2 - t) >= 0 && (x2 - t) < width) {
            for (int y = y1; y <= y2 && y < height; y++) {
                y_plane[y * width + (x2 - t)] = y_val;
            }
        }
    }

    // 画 UV 平面（2x2 子采样）
    int uv_y1 = y1 / 2;
    int uv_y2 = y2 / 2;
    int uv_x1 = x1 / 2;
    int uv_x2 = x2 / 2;
    int uv_thickness = (thickness > 1) ? (thickness / 2) : 1;
    int uv_height = height / 2;

    // UV 平面水平线
    for (int t = 0; t < uv_thickness; t++) {
        // 上边
        if ((uv_y1 + t) >= 0 && (uv_y1 + t) < uv_height) {
            for (int x = uv_x1; x <= uv_x2; x++) {
                int uv_offset = (uv_y1 + t) * width + x * 2;
                if (uv_offset + 1 < width * uv_height) {
                    uv_plane[uv_offset] = u_val;     // U
                    uv_plane[uv_offset + 1] = v_val; // V
                }
            }
        }
        // 下边
        if ((uv_y2 - t) >= 0 && (uv_y2 - t) < uv_height) {
            for (int x = uv_x1; x <= uv_x2; x++) {
                int uv_offset = (uv_y2 - t) * width + x * 2;
                if (uv_offset + 1 < width * uv_height) {
                    uv_plane[uv_offset] = u_val;
                    uv_plane[uv_offset + 1] = v_val;
                }
            }
        }
    }

    // UV 平面垂直线
    for (int t = 0; t < uv_thickness; t++) {
        // 左边
        if ((uv_x1 + t) >= 0 && (uv_x1 + t) * 2 + 1 < width) {
            for (int y = uv_y1; y <= uv_y2 && y < uv_height; y++) {
                int uv_offset = y * width + (uv_x1 + t) * 2;
                if (uv_offset + 1 < width * uv_height) {
                    uv_plane[uv_offset] = u_val;
                    uv_plane[uv_offset + 1] = v_val;
                }
            }
        }
        // 右边
        if ((uv_x2 - t) >= 0 && (uv_x2 - t) * 2 + 1 < width) {
            for (int y = uv_y1; y <= uv_y2 && y < uv_height; y++) {
                int uv_offset = y * width + (uv_x2 - t) * 2;
                if (uv_offset + 1 < width * uv_height) {
                    uv_plane[uv_offset] = u_val;
                    uv_plane[uv_offset + 1] = v_val;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    // 禁用 stdout 缓冲，确保 printf 立即输出
    setbuf(stdout, NULL);

    if (argc != 3)
    {
        printf("Usage: %s <model_path> <rtsp_url>\n", argv[0]);
        printf("Example: %s model/yolov8n.rknn rtsp://192.168.168.168:8554/test\n", argv[0]);
        return -1;
    }

    const char *model_path = argv[1];
    const char *rtsp_url = argv[2];

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int ret;
    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    // 初始化后处理
    init_post_process();

    // 初始化 YOLO 模型
    printf("Loading RKNN model: %s\n", model_path);
    ret = init_yolov8_model(model_path, &rknn_app_ctx);
    if (ret != 0)
    {
        printf("init_yolov8_model fail! ret=%d model_path=%s\n", ret, model_path);
        return -1;
    }
    printf("RKNN model loaded successfully\n");

    // 初始化 RTSP 源
    printf("Connecting to RTSP: %s\n", rtsp_url);
    rtsp_source_t rtsp_src;
    memset(&rtsp_src, 0, sizeof(rtsp_source_t));
    ret = rtsp_source_init(&rtsp_src, rtsp_url);
    if (ret != 0)
    {
        printf("rtsp_source_init fail! ret=%d\n", ret);
        release_yolov8_model(&rknn_app_ctx);
        return -1;
    }

    printf("\n========================================\n");
    printf("Starting RTSP inference loop\n");
    printf("Press Ctrl+C to stop\n");
    printf("========================================\n\n");

    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(image_buffer_t));

    int frame_count = 0;
    int detect_count = 0;

    // 推流器延迟初始化（获取第一帧后才知道分辨率）
    rtsp_streamer_t streamer;
    bool streamer_initialized = false;

    while (g_running)
    {
        // 获取一帧
        ret = rtsp_source_get_frame(&rtsp_src, &src_image);
        if (ret != 0)
        {
            printf("rtsp_source_get_frame fail! ret=%d, retrying...\n", ret);
            sleep(1);
            continue;
        }

        frame_count++;

        // 第一帧时初始化推流器
        if (!streamer_initialized) {
            printf("First frame received: %dx%d\n", src_image.width, src_image.height);
            ret = rtsp_streamer_init(&streamer, 8080, src_image.width, src_image.height);
            if (ret != 0)
            {
                printf("rtsp_streamer_init fail! ret=%d, continuing without streaming\n", ret);
            } else {
                printf("Streaming to: tcp://192.168.137.121:8080 (use VLC to connect)\n");
                printf("VLC connection string: tcp://192.168.137.121:8080\n\n");
                streamer_initialized = true;
            }
        }

        // 执行推理
        object_detect_result_list od_results;
        ret = inference_yolov8_model(&rknn_app_ctx, &src_image, &od_results);
        if (ret != 0)
        {
            printf("inference_yolov8_model fail! ret=%d\n", ret);
            continue;
        }

        // 打印检测结果（推理坐标）
        if (od_results.count > 0) {
            printf("[Frame %d] Detected %d objects:\n", frame_count, od_results.count);
            for (int i = 0; i < od_results.count; i++)
            {
                object_detect_result *det_result = &(od_results.results[i]);
                printf("  - %s @ (%d,%d)-(%d,%d) conf=%.3f\n",
                       coco_cls_to_name(det_result->cls_id),
                       det_result->box.left, det_result->box.top,
                       det_result->box.right, det_result->box.bottom,
                       det_result->prop);
            }
            detect_count++;
        }

        // 在原始 1920x1080 NV12 帧上画框（直接操作 NV12 平面）
        // 注意：inference_yolov8_model 的后处理已经将坐标映射回原始图像尺寸 (1920x1080)
        // 不需要再做 letterbox 坐标转换
        if (od_results.count > 0) {
            for (int i = 0; i < od_results.count; i++)
            {
                object_detect_result *det_result = &(od_results.results[i]);

                // 直接使用后处理返回的坐标（已经是 1920x1080 空间）
                int x1 = det_result->box.left;
                int y1 = det_result->box.top;
                int x2 = det_result->box.right;
                int y2 = det_result->box.bottom;

                // 使用新的 NV12 直接画框方法（绿色框，Phase 3 验证的颜色）
                // Y=150(亮度), U=50, V=50 (绿色)
                draw_box_nv12(src_image.virt_addr, 1920, 1080,
                             x1, y1, x2, y2,
                             4,  // thickness=4 (Phase 3 使用的线宽)
                             150, 50, 50);  // 绿色 (Y, U, V)

                printf("[Draw] Box drawn at (%d,%d)-(%d,%d) for %s\n",
                       x1, y1, x2, y2, coco_cls_to_name(det_result->cls_id));
            }
        }

        // 推流带检测框的帧
        if (streamer_initialized) {
            ret = rtsp_streamer_push_frame(&streamer, &src_image);
            if (ret != 0)
            {
                printf("rtsp_streamer_push_frame fail! ret=%d\n", ret);
            } else if (frame_count % 30 == 0) {
                printf("[Stream] Successfully pushed frame %d\n", frame_count);
            }
        }

        // 每 30 帧打印一次无检测信息（避免刷屏）
        if (od_results.count == 0 && frame_count % 30 == 0) {
            printf("[Frame %d] No objects detected\n", frame_count);
        }
    }

    printf("\n========================================\n");
    printf("Inference stopped\n");
    printf("Total frames: %d\n", frame_count);
    printf("Frames with detections: %d\n", detect_count);
    printf("========================================\n");

    // 清理资源
    if (streamer_initialized) {
        rtsp_streamer_release(&streamer);
    }
    if (src_image.virt_addr != NULL)
    {
        free(src_image.virt_addr);
    }
    rtsp_source_release(&rtsp_src);

out:
    deinit_post_process();
    ret = release_yolov8_model(&rknn_app_ctx);
    if (ret != 0)
    {
        printf("release_yolov8_model fail! ret=%d\n", ret);
    }

    return 0;
}
