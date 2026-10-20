/**
 * MiuiHomeEdit - native half of a libxposed (API 101+) module for com.miui.home.
 *
 * The launcher (HyperOS 4, RELEASE-8.01.02.7719) is a Flutter/Dart AOT app: its
 * Dart code is compiled into libapp.so and is not reachable through Java hooks.
 * Everything interesting therefore happens here, in the launcher process.
 *
 * ---------------------------------------------------------------------------
 * Framework contract (identical in LSPosed and in Vector/libxposed)
 * ---------------------------------------------------------------------------
 * The framework hooks dlopen(); when a path matches an entry in
 * META-INF/xposed/native_init.list it dlopen()s the library itself and then
 * looks up
 *
 *     extern "C" NativeOnModuleLoaded native_init(const NativeAPIEntries *);
 *
 * passing a pointer to its own hook table. The returned callback is registered
 * globally and invoked for every dlopen() that happens afterwards in this
 * process - including the one that triggered native_init - so it must be fast
 * and must not itself dlopen() anything (the framework calls it while holding
 * its registry mutex, and a nested dlopen would re-enter that mutex).
 *
 * This library is mapped by System.loadLibrary("miuihome_edit") from
 * MiuiHomeModule's static initialiser; native_init() has already run by the
 * time that call returns.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#include "aot_scanner.h"
#include "dart_hooks.h"
#include "log.h"
#include "module_config.h"

// Function pointer types of the framework's hook table. They match
// lsplant::Hook / lsplant::UnHook as exported by both LSPosed's and Vector's
// liblspd.so; the framework converts them to its internal API on the other side.
using HookFunType = int (*)(void *func, void *replace, void **backup);
using UnhookFunType = int (*)(void *func);
using NativeOnModuleLoaded = void (*)(const char *name, void *handle);

/**
 * The table the framework passes to native_init(). The layout is fixed by
 * LSPosed's and Vector's native_api.h: version 2 guarantees hookFunc and
 * unhookFunc follow the version field, and any later API additions are
 * appended, so reading these three fields is safe for every supported version.
 */
struct NativeAPIEntries {
    std::uint32_t version;
    HookFunType hookFunc;
    UnhookFunType unhookFunc;
};

namespace {

constexpr char kTargetPackage[] = "com.miui.home";
constexpr char kModuleLibrary[] = "libmiuihome_edit.so";
constexpr char kDartImage[] = "libapp.so";

// Filled in by native_init() before the callback above can ever be invoked.
std::atomic<HookFunType> g_hook{nullptr};
std::atomic<UnhookFunType> g_unhook{nullptr};
std::atomic<bool> g_in_target_process{false};

// The module may be injected after the snapshot is already mapped, and the
// dlopen() callback may never see the snapshot at all (see StartDartWatchdog),
// so this flag is the single source of truth for "hooks are installed".
std::atomic<bool> g_dart_hooks_installed{false};

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * Reads the first NUL-terminated field of /proc/self/cmdline, i.e. the process
 * name as the framework sees it. Processes launched by the activity manager
 * have no argv, so the kernel exposes argv[0] there instead.
 */
void GetProcessName(char *buffer, std::size_t size) {
    if (buffer == nullptr || size == 0) {
        return;
    }
    buffer[0] = '\0';

    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    const ssize_t length = read(fd, buffer, size - 1);
    close(fd);
    if (length <= 0) {
        buffer[0] = '\0';
        return;
    }
    buffer[length] = '\0';
}

/**
 * The launcher runs in "com.miui.home"; its auxiliary processes are named
 * "com.miui.home:<suffix>". Both are accepted: the module is loaded once per
 * process, so each one installs its own hooks, and some launcher features
 * (recents, widgets) live in a helper process rather than in the main one.
 */
bool IsTargetProcess(std::string_view process_name) {
    const std::string_view package(kTargetPackage);
    if (process_name == package) {
        return true;
    }
    return process_name.size() > package.size() &&
           process_name.compare(0, package.size(), package) == 0 &&
           process_name[package.size()] == ':';
}

/**
 * Hooks an exported symbol. Returns the trampoline to the original function, or
 * nullptr on failure.
 */
[[maybe_unused]] void *HookSymbol(void *handle, const char *symbol, void *replacement) {
    const HookFunType hook = g_hook.load(std::memory_order_acquire);
    if (handle == nullptr || symbol == nullptr || replacement == nullptr || hook == nullptr) {
        return nullptr;
    }

    dlerror();
    void *target = dlsym(handle, symbol);
    if (target == nullptr) {
        const char *error = dlerror();
        LOGW("dlsym(%s) failed: %s", symbol, error != nullptr ? error : "unknown error");
        return nullptr;
    }

    void *backup = nullptr;
    const int result = hook(target, replacement, &backup);
    if (result != 0) {
        LOGE("hook of %s (%p) failed: %d", symbol, target, result);
        return nullptr;
    }
    LOGI("hooked %s at %p (original at %p)", symbol, target, backup);
    return backup;
}

/**
 * Hooks an absolute address through the framework's lsplant hook. Dart AOT code
 * uses Dobby instead (see dart_hooks.cpp), because a Dart function needs the
 * special-register handling and musttail fallback implemented there. This
 * helper remains for native/exported targets that are easier to reach by
 * address than by symbol.
 */
[[maybe_unused]] bool HookAddress(std::uintptr_t address, void *replacement, void **backup) {
    const HookFunType hook = g_hook.load(std::memory_order_acquire);
    if (address == 0 || replacement == nullptr || hook == nullptr) {
        return false;
    }
    const int result = hook(reinterpret_cast<void *>(address), replacement, backup);
    if (result != 0) {
        LOGE("hook of %p failed: %d", reinterpret_cast<void *>(address), result);
        return false;
    }
    LOGI("hooked %p", reinterpret_cast<void *>(address));
    return true;
}

/** Removes a hook installed by HookAddress(). Mostly useful while developing. */
[[maybe_unused]] bool UnhookAddress(std::uintptr_t address) {
    const UnhookFunType unhook = g_unhook.load(std::memory_order_acquire);
    if (address == 0 || unhook == nullptr) {
        return false;
    }
    const int result = unhook(reinterpret_cast<void *>(address));
    if (result != 0) {
        LOGE("unhook of %p failed: %d", reinterpret_cast<void *>(address), result);
        return false;
    }
    return true;
}

struct ImageSearch {
    std::string_view soname;
    std::uintptr_t base;
};

int FindImageCallback(dl_phdr_info *info, std::size_t /*size*/, void *data) {
    auto *search = static_cast<ImageSearch *>(data);
    if (info->dlpi_name != nullptr && EndsWith(info->dlpi_name, search->soname)) {
        search->base = static_cast<std::uintptr_t>(info->dlpi_addr);
        return 1;  // stop iterating
    }
    return 0;
}

/**
 * Load bias of the first loaded image whose path ends with |soname|. For the
 * launcher's own libraries that is the value an RE tool reports as the module
 * base, so static offsets add up directly.
 */
[[maybe_unused]] std::uintptr_t FindImageBase(std::string_view soname) {
    ImageSearch search{soname, 0};
    dl_iterate_phdr(&FindImageCallback, &search);
    return search.base;
}

/** True for images that belong to the Dart/Flutter engine or to this module. */
bool IsEngineImage(std::string_view name) {
    return name.find("libapp") != std::string_view::npos ||
           name.find("flutter") != std::string_view::npos ||
           name.find("dart") != std::string_view::npos ||
           name.find("resource") != std::string_view::npos ||
           name.find("native_widget") != std::string_view::npos ||
           name.find("miuihome") != std::string_view::npos;
}

int LogImageCallback(dl_phdr_info *info, std::size_t /*size*/, void *data) {
    auto *count = static_cast<int *>(data);
    if (info == nullptr || info->dlpi_name == nullptr ||
        info->dlpi_name[0] == '\0') {
        return 0;
    }
    const std::string_view name(info->dlpi_name);
    if (!IsEngineImage(name)) {
        return 0;
    }
    ++*count;
    LOGI("image: %s base=%p", info->dlpi_name,
         reinterpret_cast<void *>(info->dlpi_addr));
    return 0;
}

/**
 * Logs every engine image that is currently mapped.
 *
 * This is the authoritative answer to "which libraries does this launcher
 * build actually have". HyperOS 4 (RELEASE-8.01.02.7719) ships no
 * libflutter.so at all - the engine lives in libapp_launcher.so and the
 * snapshot is mapped from inside MiuiHome.apk - so /proc/<pid>/maps alone
 * cannot show it (an APK-embedded .so is mapped as <apk> + file offset).
 * Call it with |when| describing the moment, e.g. "native_init".
 */
void LogLoadedImages(const char *when) {
    int count = 0;
    dl_iterate_phdr(&LogImageCallback, &count);
    if (count == 0) {
        LOGI("images (%s): no libapp/libflutter/resource image is mapped yet",
             when);
    } else {
        LOGI("images (%s): %d engine image(s) listed above", when, count);
    }
}

/**
 * Installs the Dart AOT hooks once libapp.so is mapped.
 *
 * Deliberately *not* gated on libflutter.so: the targeted launcher build has no
 * such library, and the previous gate silently skipped every hook there (the
 * only trace was a single "waiting for libflutter.so" line). The Dart snapshot
 * libapp.so is the only image the hooks need.
 *
 * The call is cheap while waiting (one dl_iterate_phdr pass) and idempotent
 * once installed. Dobby is configured without its symbol resolver, so
 * DobbyHook() never dlopen()s: calling it from the framework's dlopen callback
 * cannot re-enter the registry mutex.
 */
void TryInstallDartHooks(const char *reason) {
    aot::Image dart_image;
    if (!aot::FindImage(kDartImage, &dart_image)) {
        static std::atomic<bool> waiting_logged{false};
        if (!waiting_logged.exchange(true, std::memory_order_acq_rel)) {
            LOGI("%s is not mapped yet (%s); installing as soon as it appears",
                 kDartImage, reason);
            LogLoadedImages("before libapp.so");
        }
        return;
    }
    if (g_dart_hooks_installed.exchange(true, std::memory_order_acq_rel)) {
        return;  // already installed by an earlier dlopen()/watchdog pass
    }

    LOGI("%s mapped (base=%p, reason=%s)", kDartImage,
         reinterpret_cast<void *>(dart_image.base), reason);
    LogLoadedImages("libapp.so present");

    // Record the executable extent of the Dart snapshot once: every hook below
    // is resolved by scanning this span, so the addresses never have to be
    // hard-coded for one launcher build.
    const aot::Span text = aot::FindSegment(dart_image, PF_X);
    if (text.Valid()) {
        LOGI("%s: load bias=%p, .text-like span=%zu KiB at %p", kDartImage,
             reinterpret_cast<void *>(dart_image.base), text.size / 1024,
             reinterpret_cast<void *>(text.address));
    } else {
        LOGW("%s: no executable PT_LOAD segment", kDartImage);
    }

    // Segment/build-id output is always useful; the string probe is expensive
    // and only worth running when the user is actually trying to use a switch.
    miuihome::diag::DumpImage(dart_image, kDartImage);
    if (miuihome::config::AnyEnabled()) {
        // The probe scans libapp.so several times and is diagnostic-only. Run it
        // off the dlopen callback so launcher startup and the framework's
        // registry mutex are not held for the duration of the scan.
        LOGI("diagnostics: scheduling Dart string probe on a background thread");
        std::thread probe([dart_image]() {
            miuihome::diag::ProbeDartImage(dart_image);
        });
        probe.detach();
    } else {
        LOGI("diagnostics: all switches are off; Dart string probe skipped");
    }

    miuihome::dart::Install(dart_image);

    // Exported symbols (engine C API) still use HookSymbol(), which goes
    // through the framework's lsplant hook and needs no offsets:
    //
    //     void *original = HookSymbol(handle, "SomeExportedSymbol",
    //                                 reinterpret_cast<void *>(&MyReplacement));
    //
    // Nothing is hooked unless a target is configured in dart_hooks.cpp: a
    // stale offset in the launcher is a boot loop.
}

/**
 * dlopen() callback. Runs under the framework's registry mutex and before the
 * caller's dlopen() returns, so it only does cheap string work plus, at most,
 * one hook installation. It must never dlopen() by itself.
 */
void OnLibraryLoaded(const char *name, void *handle) {
    if (name == nullptr || handle == nullptr) {
        return;
    }
    if (!g_in_target_process.load(std::memory_order_acquire)) {
        return;
    }

    const std::string_view path(name);
    if (EndsWith(path, kModuleLibrary)) {
        return;  // this module's own library
    }

    // Retry on every dlopen() until the snapshot is present. This also covers
    // the case where the module was injected after libapp.so had already been
    // mapped and no further dlopen() will mention it (the watchdog covers the
    // remaining case where the callback never fires at all).
    if (!g_dart_hooks_installed.load(std::memory_order_acquire)) {
        TryInstallDartHooks(name);
    }

    if (EndsWith(path, kDartImage)) {
        LOGI("Dart AOT image dlopen: %s (handle=%p)", name, handle);
        return;
    }
    LOGD("dlopen: %s", name);
}

/**
 * Installs the hooks even if no dlopen() event is ever reported for libapp.so.
 *
 * The framework's callback is the normal path, but it only fires for dlopen()
 * calls that go through the framework. The HyperOS launcher maps its snapshot
 * from inside MiuiHome.apk, so the event can be missing entirely - which is
 * exactly what the device logs showed: native_init ran, yet neither libapp nor
 * libflutter was ever reported. Polling dl_iterate_phdr for 20 s is cheap and
 * safe: an install that happens after the first frame only leaves the affected
 * widgets in their original state until they are rebuilt.
 */
void StartDartWatchdog() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (g_dart_hooks_installed.load(std::memory_order_acquire)) {
        return;
    }
    std::thread([] {
        for (int attempt = 1; attempt <= 40; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (g_dart_hooks_installed.load(std::memory_order_acquire)) {
                return;
            }
            aot::Image image;
            if (aot::FindImage(kDartImage, &image)) {
                LOGI("watchdog: %s appeared without a dlopen callback (attempt %d)",
                     kDartImage, attempt);
                TryInstallDartHooks("watchdog");
                return;
            }
        }
        LOGW("watchdog: %s still not mapped after 20 s; giving up", kDartImage);
        LogLoadedImages("watchdog timeout");
    }).detach();
}

}  // namespace

/**
 * Native entry point. The framework calls this immediately after mapping this
 * library, before System.loadLibrary() returns, and expects either a dlopen
 * callback or nullptr.
 *
 * Returning nullptr does not fail the module: it only means "nothing to observe
 * in this process", which is the correct answer for every process that is not
 * the launcher's main process.
 */
extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) {
        LOGE("native_init(NULL): the framework passed no API table");
        return nullptr;
    }

    char process_name[256] = {};
    GetProcessName(process_name, sizeof(process_name));
    const bool target = IsTargetProcess(std::string_view(process_name));

    // Open the diagnostics file before the first log line. On a rooted device
    // the control panel collects it through su, so it survives logcat buffer
    // rotation and does not depend on the panel being allowed to read logcat.
    miuihome::diag::Initialize(process_name);
    miuihome::diag::DumpEnvironment();

    LOGI("native_init: framework API v%u, process '%s'%s",
         entries->version,
         process_name[0] != '\0' ? process_name : "<unknown>",
         target ? " (target)" : "");
    LOGI("native_init: hookFunc=%p unhookFunc=%p",
         reinterpret_cast<void *>(entries->hookFunc),
         reinterpret_cast<void *>(entries->unhookFunc));

    if (entries->version < 2 || entries->hookFunc == nullptr) {
        LOGE("unsupported native API v%u: hookFunc is required", entries->version);
        return nullptr;
    }

    static std::atomic<bool> registered{false};
    if (registered.exchange(true, std::memory_order_acq_rel)) {
        LOGW("native_init called again; keeping the first registration");
        return target ? &OnLibraryLoaded : nullptr;
    }

    g_hook.store(entries->hookFunc, std::memory_order_release);
    g_unhook.store(entries->unhookFunc, std::memory_order_release);
    g_in_target_process.store(target, std::memory_order_release);

    if (!target) {
        LOGI("not com.miui.home; no hooks will be installed in this process");
        return nullptr;
    }

    // Read /sdcard/Android/data/com.miui.home/files/miuihome_edit.json (or one
    // of the fallbacks listed in module_config.cpp) before installing anything.
    // A missing/corrupt file leaves all six switches off; the hook wrappers
    // re-check the file lazily, so the control panel can be used afterwards.
    miuihome::config::Initialize();

    // Late injection: libapp.so may already be mapped, in which case no dlopen
    // event will ever arrive for it. Try once here, list what is present (the
    // only reliable image list on this launcher build) and then keep a slow
    // watchdog for builds whose loader bypasses the framework's dlopen hook.
    LogLoadedImages("native_init");
    TryInstallDartHooks("native_init");
    StartDartWatchdog();
    return &OnLibraryLoaded;
}


