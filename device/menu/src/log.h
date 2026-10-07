#pragma once

#include <android/log.h>

#define NH_TAG "NHMENU"

#define NH_LOGI(...) __android_log_print(ANDROID_LOG_INFO, NH_TAG, __VA_ARGS__)
#define NH_LOGW(...) __android_log_print(ANDROID_LOG_WARN, NH_TAG, __VA_ARGS__)
#define NH_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, NH_TAG, __VA_ARGS__)
#define NH_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, NH_TAG, __VA_ARGS__)
