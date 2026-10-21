/**
 * dart_hooks.h - Dobby-based inline hooks for Dart AOT code in libapp.so.
 *
 * The targets are build-specific: fill in the image-relative vaddrs and object
 * pool offsets in dart_hooks.cpp for the launcher version you are supporting.
 * Install() is called once the Dart snapshot image (libapp.so) is mapped. It is
 * deliberately not gated on libflutter.so: the targeted launcher build ships no
 * such library.
 */

#pragma once

#include <cstdint>

#include "aot_scanner.h"

namespace miuihome::dart {

/**
 * Installs every configured hook. Never throws and never aborts: an address or
 * an object that does not validate is logged and skipped, so a mismatch leaves
 * the launcher running with its original behaviour (fail-open).
 */
void Install(const aot::Image &image);

/**
 * Re-validates the installed hooks against the live process. Call it
 * periodically: if libapp.so was re-mapped (new engine/isolate) the hooks are
 * reinstalled on the new image and every isolate-scoped cache is dropped; if
 * the executable pages were re-faulted from MiuiHome.apk and the original
 * prologues are back, the captured Dobby patches are written again. Cheap
 * enough for a one-second watchdog: it only iterates the loader's phdr list
 * and compares 16 bytes per hook.
 */
void EnsureHealthy();

}  // namespace miuihome::dart
