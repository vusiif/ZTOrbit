cmake_minimum_required(VERSION 3.21.1)
project(ztgui_media VERSION 2.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(ZTGUIPluginApi 0.5 EXACT CONFIG REQUIRED)
find_package(ZTGUIMediaApi 2 CONFIG REQUIRED)
find_package(Qt6 6.8 REQUIRED COMPONENTS Network Gui Multimedia)
include(engine-sdk/FFmpeg.cmake)
add_subdirectory(engine-sdk)
add_subdirectory(examples/flip)
include(FetchContent)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(opus URL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
    URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1)
FetchContent_MakeAvailable(opus)
add_library(ztmedia_engine_runtime STATIC service/DeviceManager.cpp service/SessionEngine.cpp service/ClipPlayer.cpp service/VideoCodec.cpp)
target_link_libraries(ztmedia_engine_runtime PUBLIC ZTGUI::MediaTypes ZTGUI::MediaEngine ZTGUI::Protocol Qt6::Multimedia Opus::opus
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avfilter FFmpeg::avdevice FFmpeg::swscale FFmpeg::swresample)
add_executable(ztgui_media_worker service/main.cpp)
target_link_libraries(ztgui_media_worker PRIVATE ztmedia_engine_runtime)
add_dependencies(ztgui_media_worker ztmedia_flip)
if(MSVC)
    target_compile_options(ztmedia_engine_runtime PRIVATE /utf-8 /permissive-)
    target_compile_options(ztgui_media_worker PRIVATE /utf-8)
endif()
include(cmake/BundleExtensions.cmake)
set(runtime_files "runtime/bin/openh264.dll=${MEDIA_FFMPEG_ROOT}/bin/openh264.dll")
set(runtime_overrides "")
foreach(dll IN LISTS MEDIA_FFMPEG_DLLS)
    get_filename_component(name "${dll}" NAME)
    if(name MATCHES "^(avfilter|avdevice)-")
        list(APPEND runtime_files "runtime/bin/${name}=${dll}")
    else()
        list(APPEND runtime_overrides "runtime/bin/${name}=${dll}")
    endif()
endforeach()
ztgui_package_plugin(ztgui_media_worker HOST client ID media NAME "媒体服务" VERSION "2.0.0"
    API "${ZTGUI_API_VERSION}" IID "ztgui-service/1" SERVICE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/service-manifest.json"
    RUNTIME_QT_MODULES Gui Network Multimedia RUNTIME_FILES ${runtime_files} RUNTIME_OVERRIDE_FILES ${runtime_overrides}
    RESOURCE_FILES "extensions.json=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/extensions.json" ${extension_resources}
    RUNTIME_PROVENANCE "${MEDIA_FFMPEG_ROOT}/actual-build.json"
    LICENSE_FILES "licenses/Opus-COPYING.txt=${opus_SOURCE_DIR}/COPYING"
      "licenses/FFmpeg/COPYING.LGPLv2.1=${MEDIA_FFMPEG_ROOT}/COPYING.LGPLv2.1"
      "licenses/FFmpeg/LICENSE.md=${MEDIA_FFMPEG_ROOT}/LICENSE.md"
      "licenses/FFmpeg/actual-build.json=${MEDIA_FFMPEG_ROOT}/actual-build.json"
      "licenses/OpenH264/LICENSE=${MEDIA_FFMPEG_ROOT}/LICENSE")
install(FILES "$<TARGET_FILE_DIR:ztgui_media_worker>/ztgui_media_worker.ztplugin" DESTINATION plugins)
option(MEDIA_BUILD_TESTS "Build media engine tests" OFF)
if(MEDIA_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/media" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
