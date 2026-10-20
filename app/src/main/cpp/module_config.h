/**
 * module_config.h - the five user-facing switches, read from a JSON file.
 *
 * The launcher is a Flutter AOT process, so the module cannot rely on a fully
 * initialised Java layer inside com.miui.home. The control panel therefore
 * writes a small JSON document and this code reads it directly.
 *
 * All five features default to off: a missing, unreadable or malformed file
 * leaves every bit clear, which means every hook tail-calls its original.
 */

#pragma once

#include <cstdint>

namespace miuihome::config {

/** One bit per control-panel switch. Keep the JSON keys in sync with Kotlin. */
enum class Feature : std::uint32_t {
    kHideDrawerAllTab = 1u << 0,
    kHideEditButton = 1u << 1,
    kHideSettingsGear = 1u << 2,
    kHideSearchBox = 1u << 3,
    kHideSwipeUpArrow = 1u << 4,
};

/**
 * Reads the newest valid configuration file and publishes its bits.
 *
 * Safe to call more than once and from any thread. Called from native_init()
 * before the Dart hooks are installed, and again lazily if a hook runs before
 * the first explicit call.
 */
void Initialize();

/**
 * Cheap, rate-limited reload check. Call this at the top of a hook wrapper:
 * it performs at most one stat() pass over the candidate paths every two
 * seconds and re-reads the JSON only when a file's size/mtime/inode changed.
 */
void MaybeReload();

/** Returns the currently published value. Missing keys are false. */
bool IsEnabled(Feature feature) noexcept;

/** True when at least one of the five switches is on. */
bool AnyEnabled() noexcept;

}  // namespace miuihome::config
