cmake_minimum_required(VERSION 3.22.1)
project(xcamvr LANGUAGES C CXX)

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(xcamvr SHARED main.cpp)
target_compile_options(xcamvr PRIVATE -Wall -Wno-unused-parameter)
set_target_properties(xcamvr PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(xcamvr
    native_app_glue
    OpenXR::openxr_loader
    android
    jnigraphics
    EGL
    GLESv3
    log)
