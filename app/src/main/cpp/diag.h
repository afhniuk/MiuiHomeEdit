/**
 * diag.h - persistent, collectable diagnostics for the launcher process.
 *
 * Every LOGx() macro in log.h goes through Writef(), so the same line is sent
 * to logcat and appended to a small file the control panel can collect through
 * root. That matters here: logcat can be cleared, may be rate-limited and may
 * not be readable from the panel on newer Android releases, while a file in the
 * launcher's own files/ directory survives and is readable by root.
 *
 * The file is deliberately plain text. A diagnostic report is meant to be read
 * by a human and attached to a bug report, not parsed by another program.
 */

#pragma once

#include <android/log.h>

#include <cstddef>

namespace aot {
struct Image;
}  // namespace aot

namespace miuihome::diag {

/**
 * Records the process name and opens the log file. Safe to call more than once
 * and from any thread. Writef() performs the same lazy setup when the module
 * logs before this call.
 */
void Initialize(const char* process_name);

/** Logs to logcat and, when available, the diagnostics file. */
void Writef(int priority, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Writes one boot-time block: Android/ROM properties, ABI, page size, UID, PID,
 * module path and the paths of the two Flutter images when they are mapped.
 */
void DumpEnvironment();

/** Logs every PT_LOAD segment and the GNU build-id of a mapped ELF image. */
void DumpImage(const aot::Image& image, const char* name);

/**
 * One-shot static probe for the strings that identify the six UI features.
 *
 * For each anchor this logs the raw hit address, the containing image offset,
 * a plausible Dart string object header (class id and length) and any 8-byte
 * aligned value in the image that points at that object. Those pool-like
 * references are exactly what is needed to derive the pool offsets used by
 * dart_hooks.cpp, so this turns a "nothing happens" report into actionable
 * data without requiring a second APK build.
 *
 * It is intentionally expensive (a few linear scans of libapp.so) and is only
 * called once, when at least one switch is enabled.
 */
void ProbeDartImage(const aot::Image& image);

}  // namespace miuihome::diag
