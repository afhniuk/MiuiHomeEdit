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

}  // namespace miuihome::dart
