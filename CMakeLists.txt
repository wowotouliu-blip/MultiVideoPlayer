cmake_minimum_required(VERSION 3.16)
project(MultiWindowVideoPlayer VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g" CACHE STRING "" FORCE)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESv2 REQUIRED glesv2)
pkg_check_modules(LIBVA REQUIRED libva)
pkg_check_modules(LIBVA_DRM REQUIRED libva-drm)
pkg_check_modules(LIBAVCODEC REQUIRED libavcodec)
pkg_check_modules(LIBAVFORMAT REQUIRED libavformat)
pkg_check_modules(LIBAVUTIL REQUIRED libavutil)
pkg_check_modules(LIBSWRESAMPLE REQUIRED libswresample)
pkg_check_modules(LIBDECOR REQUIRED libdecor-0)

# Source files
set(SOURCES
    src/main.cpp
    src/wayland_window.cpp
    src/vaapi_decoder.cpp
    src/audio_output.cpp
    src/egl_renderer.cpp
    src/player_window.cpp
)

# Add executable
add_executable(multi_video_player ${SOURCES})

target_include_directories(multi_video_player PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESv2_INCLUDE_DIRS}
    ${LIBVA_INCLUDE_DIRS}
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVFORMAT_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
    ${LIBSWRESAMPLE_INCLUDE_DIRS}
    ${LIBDECOR_INCLUDE_DIRS}
    /usr/include/drm
)

target_link_libraries(multi_video_player PRIVATE
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESv2_LIBRARIES}
    ${LIBVA_LIBRARIES}
    ${LIBVA_DRM_LIBRARIES}
    ${LIBAVCODEC_LIBRARIES}
    ${LIBAVFORMAT_LIBRARIES}
    ${LIBAVUTIL_LIBRARIES}
    ${LIBSWRESAMPLE_LIBRARIES}
    ${LIBDECOR_LIBRARIES}
    pthread
    dl
)

target_compile_options(multi_video_player PRIVATE -Wall -Wextra)
