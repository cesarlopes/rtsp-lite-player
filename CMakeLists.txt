cmake_minimum_required(VERSION 3.20)

project(rtsp_lite_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
    )
endif()

add_executable(rtsp_lite_player
    src/main.cpp
    src/app.rc
)

set_target_properties(rtsp_lite_player PROPERTIES
    OUTPUT_NAME "RtspLitePlayer"
)

target_link_libraries(rtsp_lite_player PRIVATE
    nlohmann_json::nlohmann_json
)

if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(rtsp_lite_player PRIVATE PkgConfig::FFMPEG)
else()
    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h REQUIRED)
    find_path(AVCODEC_INCLUDE_DIR libavcodec/codec_par.h REQUIRED)
    find_path(AVUTIL_INCLUDE_DIR libavutil/error.h REQUIRED)
    find_path(SWSCALE_INCLUDE_DIR libswscale/swscale.h REQUIRED)

    find_library(AVFORMAT_LIBRARY avformat REQUIRED)
    find_library(AVCODEC_LIBRARY avcodec REQUIRED)
    find_library(AVUTIL_LIBRARY avutil REQUIRED)
    find_library(SWSCALE_LIBRARY swscale REQUIRED)

    target_include_directories(rtsp_lite_player PRIVATE
        ${AVFORMAT_INCLUDE_DIR}
        ${AVCODEC_INCLUDE_DIR}
        ${AVUTIL_INCLUDE_DIR}
        ${SWSCALE_INCLUDE_DIR}
    )
    target_link_libraries(rtsp_lite_player PRIVATE
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWSCALE_LIBRARY}
    )
endif()

if(WIN32)
    target_compile_definitions(rtsp_lite_player PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(rtsp_lite_player PRIVATE user32 gdi32)
    target_link_options(rtsp_lite_player PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
endif()
