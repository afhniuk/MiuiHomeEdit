/**
 * log.h - the module's logging helpers.
 *
 * Every line goes to logcat *and* to the persistent diagnostics file managed by
 * diag.cpp. Keeping the macros in one place means main.cpp, module_config.cpp
 * and dart_hooks.cpp cannot accidentally use a log path that the control panel
 * cannot collect.
 *
 * Per-dlopen tracing is still compiled out of release builds.
 */

#pragma once

#include <android/log.h>

#include "diag.h"

#define LOG_TAG "MiuiHomeEdit"
#define LOGI(...) miuihome::diag::Writef(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) miuihome::diag::Writef(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) miuihome::diag::Writef(ANDROID_LOG_ERROR, __VA_ARGS__)
#ifdef NDEBUG
// Release builds stay silent on the per-dlopen trace; a launcher process
// dlopen()s a lot of libraries.
#define LOGD(...) ((void) 0)
#else
// Keep the high-volume dlopen trace on logcat only. Writing every library load
// to the diagnostics file would bloat it and add latency during startup.
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
