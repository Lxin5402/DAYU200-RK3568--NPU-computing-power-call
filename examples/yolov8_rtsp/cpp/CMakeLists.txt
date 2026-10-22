cmake_minimum_required(VERSION 3.10)

project(yolov8_rtsp_demo)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

set(rknpu_yolov8_file rknpu2/yolov8_zero_copy.cc)

if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    add_definitions(-DRV1106_1103)
    set(rknpu_yolov8_file rknpu2/yolov8_rv1106_1103.cc)
    #dma
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/allocator/dma)
endif()

if(TARGET_SOC STREQUAL "rk1808" OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126")
    add_definitions(-DRKNPU1)
    set(rknpu_yolov8_file rknpu1/yolov8.cc)
endif()

# ========== 新增：GStreamer 依赖（交叉编译版本，不使用 pkg-config）==========
# 从板端复制的 GStreamer 头文件路径
include_directories(
    /usr/include/gstreamer-1.0
    /usr/include/glib-2.0
    /usr/lib/aarch64-linux-gnu/glib-2.0/include
)

# 添加库搜索路径
link_directories(
    /usr/lib/aarch64-linux-gnu
    /lib/aarch64-linux-gnu
)

# 链接库直接使用库名，让交叉编译器在 sysroot 中查找
set(GSTREAMER_LIBS
    gstreamer-1.0
    gstapp-1.0
    gstallocators-1.0
    gstbase-1.0
    gobject-2.0
    glib-2.0
    dw
    elf
    unwind
)
# ==========================================

# 使用官方目录结构的相对路径（从 examples/yolov8_rtsp/cpp/ 出发）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

# ========== 修改：添加 rtsp_source.cc ==========
add_executable(${PROJECT_NAME}
    main.cc
    postprocess.cc
    rtsp_source.cc
    ${rknpu_yolov8_file}
)

# ========== 修改：添加 ZERO_COPY 宏定义 ==========
target_compile_definitions(${PROJECT_NAME} PRIVATE ZERO_COPY)

# ========== 修改：添加 GStreamer 库链接 ==========
target_link_libraries(${PROJECT_NAME}
    imageutils
    fileutils
    imagedrawing
    ${LIBRKNNRT}
    ${GSTREAMER_LIBS}
    dl
)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}
    log
)
endif()

message(STATUS "!!!!!!!!!!!CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)

install(TARGETS ${PROJECT_NAME} DESTINATION .)
