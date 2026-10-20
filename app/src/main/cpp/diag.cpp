#include "diag.h"

#include <android/api-level.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string_view>
#include <vector>

#include "aot_scanner.h"

namespace miuihome::diag {
namespace {

constexpr char kTag[] = "MiuiHomeEdit";
constexpr std::size_t kMaxFileBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaxLineBytes = 2304u;
constexpr std::size_t kPathBytes = 512u;
constexpr std::size_t kProcessBytes = 256u;
constexpr std::size_t kPackageBytes = 128u;

std::mutex g_mutex;
int g_fd = -1;
// Extra descriptors for mirror copies. Every line is written to all of them.
// Slot 0 is the launcher's external files dir, slot 1 the public
// /sdcard/MiuiHomeEdit dir; either may stay closed when the path is not
// writable, and the primary log is unaffected.
constexpr int kMirrorSlots = 2;
int g_mirror_fds[kMirrorSlots] = {-1, -1};
bool g_open_attempted = false;
bool g_environment_dumped = false;
char g_path[kPathBytes] = {};
char g_mirror_paths[kMirrorSlots][kPathBytes] = {};
char g_process[kProcessBytes] = {};
char g_package[kPackageBytes] = {};
uid_t g_uid = 0;
pid_t g_pid = 0;

void CopyText(char* destination, std::size_t size, const char* source) {
    if (destination == nullptr || size == 0) {
        return;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    std::snprintf(destination, size, "%s", source);
}

/** Fills |buffer| with argv[0] from /proc/self/cmdline. */
void ReadProcessName(char* buffer, std::size_t size) {
    if (buffer == nullptr || size == 0) {
        return;
    }
    buffer[0] = '\0';
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    const ssize_t count = read(fd, buffer, size - 1);
    close(fd);
    if (count <= 0) {
        buffer[0] = '\0';
        return;
    }
    buffer[count] = '\0';
}

/**
 * The package is the part before ':' in an Android process name. For
 * "com.miui.home:widget" this is "com.miui.home", which is the only data
 * directory the launcher process can write without extra permissions.
 */
void DerivePackage() {
    if (g_process[0] == '\0') {
        return;
    }
    std::size_t length = 0;
    while (g_process[length] != '\0' && g_process[length] != ':' &&
           length + 1 < sizeof(g_package)) {
        ++length;
    }
    std::memcpy(g_package, g_process, length);
    g_package[length] = '\0';
}

/** Writes the whole buffer; short writes are retried, errors are dropped. */
void WriteTo(int fd, const char* text, std::size_t size) {
    if (fd < 0 || text == nullptr || size == 0) {
        return;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, text + written, size - written);
        if (count <= 0) {
            break;
        }
        written += static_cast<std::size_t>(count);
    }
}

/** Every line goes to the primary log and to every open mirror copy. */
void WriteDirectLocked(const char* text, std::size_t size) {
    if (text == nullptr || size == 0) {
        return;
    }
    WriteTo(g_fd, text, size);
    for (int slot = 0; slot < kMirrorSlots; ++slot) {
        WriteTo(g_mirror_fds[slot], text, size);
    }
}

/**
 * Opens |path| for appending, moving an oversized file to <path>.1 first.
 * Returns -1 when the path cannot be used (missing directory, SELinux, ...).
 */
int OpenRotated(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }

    struct stat info = {};
    if (stat(path, &info) == 0 &&
        static_cast<std::uint64_t>(info.st_size) > kMaxFileBytes) {
        char backup[kPathBytes] = {};
        std::snprintf(backup, sizeof(backup), "%s.1", path);
        rename(path, backup);
    }

    return open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

/** Opens the primary log and remembers its path. */
bool TryOpenPathLocked(const char* path) {
    const int fd = OpenRotated(path);
    if (fd < 0) {
        return false;
    }
    g_fd = fd;
    CopyText(g_path, sizeof(g_path), path);
    return true;
}

/**
 * Creates the directory that contains |path|, including one missing parent.
 * Android ships /sdcard/Android/data/<pkg> but not its files/ child when the
 * app itself never wrote there.
 */
void EnsureParentDirectory(const char* path) {
    char directory[kPathBytes] = {};
    CopyText(directory, sizeof(directory), path);
    char* slash = std::strrchr(directory, '/');
    if (slash == nullptr || slash == directory) {
        return;
    }
    *slash = '\0';
    if (mkdir(directory, 0770) == 0 || errno == EEXIST) {
        return;
    }

    char* parent = std::strrchr(directory, '/');
    if (parent == nullptr || parent == directory) {
        return;
    }
    *parent = '\0';
    mkdir(directory, 0770);  // .../Android/data/<pkg>
    *parent = '/';
    mkdir(directory, 0770);  // .../Android/data/<pkg>/files
}

/**
 * Opens a mirror copy of the log in |path| into |slot|.
 *
 * The private /data/user/<user>/<pkg>/files log stays the primary one, but on
 * HyperOS 4 + KernelSU even root is denied that directory (app_data_file is not
 * readable from the su domain), which made every native log invisible to the
 * control panel. Two copies are attempted: the launcher's own external files
 * dir and the public /sdcard/MiuiHomeEdit dir. The latter is readable by the
 * control panel through all-files access even on Android 11+, where
 * Android/data of other packages is hidden from that permission.
 *
 * Failures are silent: the private log and logcat still work.
 */
bool TryOpenMirrorLocked(const char* path, int slot) {
    if (path == nullptr || path[0] == '\0' || slot < 0 || slot >= kMirrorSlots ||
        g_mirror_fds[slot] >= 0) {
        return false;
    }
    EnsureParentDirectory(path);
    const int fd = OpenRotated(path);
    if (fd < 0) {
        return false;
    }
    g_mirror_fds[slot] = fd;
    CopyText(g_mirror_paths[slot], kPathBytes, path);
    return true;
}

void GetProp(const char* name, char* value, std::size_t size) {
    if (value == nullptr || size == 0) {
        return;
    }
    value[0] = '\0';
    if (name != nullptr) {
        __system_property_get(name, value);
    }
}

void WriteSessionHeaderLocked() {
    struct utsname uts = {};
    uname(&uts);

    char android_release[PROP_VALUE_MAX] = {};
    char build_fingerprint[PROP_VALUE_MAX] = {};
    GetProp("ro.build.version.release", android_release,
            sizeof(android_release));
    GetProp("ro.build.fingerprint", build_fingerprint,
            sizeof(build_fingerprint));

    char header[2048] = {};
    std::snprintf(
        header, sizeof(header),
        "===== MiuiHomeEdit native diagnostics =====\n"
        "diagnostics_format=1 compiled=%s %s\n"
        "process=%s pid=%d uid=%d package=%s\n"
        "android_release=%s sdk=%d compile_api=%d page_size=%ld\n"
        "kernel=%s %s %s\n"
        "fingerprint=%s\n"
        "pointer_size=%zu\n"
        "===========================================\n",
        __DATE__, __TIME__, g_process, static_cast<int>(g_pid),
        static_cast<int>(g_uid),
        g_package[0] != '\0' ? g_package : "<unknown>",
        android_release[0] != '\0' ? android_release : "<unknown>",
        android_get_device_api_level(), __ANDROID_API__,
        static_cast<long>(sysconf(_SC_PAGESIZE)), uts.sysname, uts.release,
        uts.machine, build_fingerprint[0] != '\0' ? build_fingerprint
                                                   : "<unknown>",
        sizeof(void*));
    WriteDirectLocked(header, std::strlen(header));
}

void OpenLocked() {
    if (g_open_attempted) {
        return;
    }
    g_open_attempted = true;

    if (g_process[0] == '\0') {
        ReadProcessName(g_process, sizeof(g_process));
    }
    DerivePackage();
    g_uid = getuid();
    g_pid = getpid();

    const int user = static_cast<int>(g_uid / 100000);
    char private_path[kPathBytes] = {};
    char external_path[kPathBytes] = {};
    char public_path[kPathBytes] = {};
    bool opened = false;

    if (g_package[0] != '\0') {
        std::snprintf(private_path, sizeof(private_path),
                      "/data/user/%d/%s/files/miuihome_edit.log", user,
                      g_package);
        std::snprintf(external_path, sizeof(external_path),
                      "/storage/emulated/%d/Android/data/%s/files/"
                      "miuihome_edit.log",
                      user, g_package);
        // Readable by the control panel on every Android version, unlike
        // Android/data of another package.
        std::snprintf(public_path, sizeof(public_path),
                      "/storage/emulated/%d/MiuiHomeEdit/miuihome_edit.log",
                      user);
        opened = TryOpenPathLocked(private_path);
        if (!opened) {
            opened = TryOpenPathLocked(external_path);
        }
    }
    if (!opened) {
        opened = TryOpenPathLocked("/data/local/tmp/miuihome_edit.log");
    }

    if (opened) {
        // Always keep external copies as well, unless that path is already the
        // primary one. They are what make the log collectable on ROMs that deny
        // root access to the launcher's private directory.
        if (external_path[0] != '\0' &&
            std::strcmp(g_path, external_path) != 0) {
            TryOpenMirrorLocked(external_path, 0);
        }
        if (public_path[0] != '\0' && std::strcmp(g_path, public_path) != 0) {
            TryOpenMirrorLocked(public_path, 1);
        }
        char mirrors[2 * kPathBytes + 8] = {};
        std::size_t used = 0;
        for (int slot = 0; slot < kMirrorSlots; ++slot) {
            if (g_mirror_fds[slot] < 0 || used >= sizeof(mirrors)) {
                continue;
            }
            const int count = std::snprintf(
                mirrors + used, sizeof(mirrors) - used, "%s%s",
                used == 0 ? "" : ", ", g_mirror_paths[slot]);
            if (count > 0 &&
                static_cast<std::size_t>(count) < sizeof(mirrors) - used) {
                used += static_cast<std::size_t>(count);
            }
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "diagnostics: native log is %s (mirror: %s)", g_path,
                            used > 0 ? mirrors : "none");
        WriteSessionHeaderLocked();
    } else {
        __android_log_print(
            ANDROID_LOG_WARN, kTag,
            "diagnostics: no writable native log path (uid=%d package=%s); "
            "logcat only",
            static_cast<int>(g_uid),
            g_package[0] != '\0' ? g_package : "<unknown>");
        // Stay logcat-only for the rest of this process. Retrying on every log
        // line would multiply SELinux denials without ever changing the path.
    }
}

const char* PriorityName(int priority) {
    switch (priority) {
        case ANDROID_LOG_VERBOSE:
            return "V";
        case ANDROID_LOG_DEBUG:
            return "D";
        case ANDROID_LOG_INFO:
            return "I";
        case ANDROID_LOG_WARN:
            return "W";
        case ANDROID_LOG_ERROR:
            return "E";
        case ANDROID_LOG_FATAL:
            return "F";
        default:
            return "?";
    }
}

void AppendHex(char* output, std::size_t size, std::size_t* offset,
               const std::uint8_t* bytes, std::size_t count) {
    if (output == nullptr || offset == nullptr || bytes == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < count && *offset + 2 < size; ++i) {
        const int written = std::snprintf(output + *offset, size - *offset,
                                          "%02x", bytes[i]);
        if (written <= 0) {
            return;
        }
        *offset += static_cast<std::size_t>(written);
    }
}

}  // namespace


void Initialize(const char* process_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pid == 0) {
        g_pid = getpid();
        g_uid = getuid();
    }
    if (process_name != nullptr && process_name[0] != '\0') {
        CopyText(g_process, sizeof(g_process), process_name);
    } else if (g_process[0] == '\0') {
        ReadProcessName(g_process, sizeof(g_process));
    }
    DerivePackage();
    OpenLocked();
}

void Writef(int priority, const char* format, ...) {
    char message[kMaxLineBytes] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    __android_log_write(priority, kTag, message);

    struct timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm local = {};
    localtime_r(&now.tv_sec, &local);

    char line[kMaxLineBytes + 128] = {};
    std::snprintf(line, sizeof(line),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03ld %5d %5ld %s %s\n",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  now.tv_nsec / 1000000L, static_cast<int>(getpid()),
                  static_cast<long>(syscall(SYS_gettid)),
                  PriorityName(priority), message);

    std::lock_guard<std::mutex> lock(g_mutex);
    OpenLocked();
    WriteDirectLocked(line, std::strlen(line));
}

void DumpEnvironment() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_environment_dumped) {
            return;
        }
        g_environment_dumped = true;
    }

    Writef(ANDROID_LOG_INFO,
           "diag: process=%s pid=%d uid=%d euid=%d ppid=%d pages=%ld",
           g_process[0] != '\0' ? g_process : "<unknown>",
           static_cast<int>(getpid()), static_cast<int>(getuid()),
           static_cast<int>(geteuid()), static_cast<int>(getppid()),
           static_cast<long>(sysconf(_SC_PAGESIZE)));
    Writef(ANDROID_LOG_INFO, "diag: runtime api=%d compile api=%d abi=%s",
           android_get_device_api_level(), __ANDROID_API__,
#if defined(__aarch64__)
           "arm64-v8a"
#elif defined(__arm__)
           "armeabi-v7a"
#else
           "other"
#endif
    );

    struct utsname uts = {};
    if (uname(&uts) == 0) {
        Writef(ANDROID_LOG_INFO, "diag: kernel %s %s %s", uts.sysname,
               uts.release, uts.machine);
    }

    Dl_info module_info = {};
    if (dladdr(reinterpret_cast<const void*>(&DumpEnvironment),
               &module_info) != 0) {
        Writef(ANDROID_LOG_INFO, "diag: module path=%s base=%p",
               module_info.dli_fname != nullptr ? module_info.dli_fname
                                                : "<unknown>",
               module_info.dli_fbase);
    }

    const char* const properties[] = {
        "ro.build.version.release",
        "ro.build.version.sdk",
        "ro.build.version.security_patch",
        "ro.build.fingerprint",
        "ro.build.version.incremental",
        "ro.product.manufacturer",
        "ro.product.model",
        "ro.product.device",
        "ro.product.cpu.abilist",
        "ro.mi.os.version.name",
        "ro.mi.os.version.code",
        "ro.miui.ui.version.name",
        "ro.miui.ui.version.code",
        "persist.sys.timezone",
    };
    for (const char* name : properties) {
        char value[PROP_VALUE_MAX] = {};
        GetProp(name, value, sizeof(value));
        Writef(ANDROID_LOG_INFO, "diag: prop %s=%s", name,
               value[0] != '\0' ? value : "<unset>");
    }

    const char* const images[] = {"libflutter.so", "libapp.so",
                                  "libmiuihome_edit.so"};
    for (const char* name : images) {
        aot::Image image;
        if (aot::FindImage(name, &image)) {
            Writef(ANDROID_LOG_INFO, "diag: image %s mapped base=%p", name,
                   reinterpret_cast<void*>(image.base));
        } else {
            Writef(ANDROID_LOG_WARN, "diag: image %s not mapped", name);
        }
    }
}


void DumpImage(const aot::Image& image, const char* name) {
    const char* const label = name != nullptr ? name : "<image>";
    if (!image.Valid()) {
        Writef(ANDROID_LOG_WARN, "diag: %s is not mapped", label);
        return;
    }

    Writef(ANDROID_LOG_INFO, "diag: %s base=%p phnum=%zu", label,
           reinterpret_cast<void*>(image.base), image.phnum);

    for (std::size_t i = 0; i < image.phnum; ++i) {
        const ElfW(Phdr)& phdr = image.phdrs[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        char flags[4] = {
            (phdr.p_flags & PF_R) != 0 ? 'R' : '-',
            (phdr.p_flags & PF_W) != 0 ? 'W' : '-',
            (phdr.p_flags & PF_X) != 0 ? 'X' : '-',
            '\0',
        };
        Writef(ANDROID_LOG_INFO,
               "diag: %s PT_LOAD %s vaddr=0x%zx memsz=0x%zx filesz=0x%zx",
               label, flags, static_cast<std::size_t>(phdr.p_vaddr),
               static_cast<std::size_t>(phdr.p_memsz),
               static_cast<std::size_t>(phdr.p_filesz));
    }

    for (std::size_t i = 0; i < image.phnum; ++i) {
        const ElfW(Phdr)& phdr = image.phdrs[i];
        if (phdr.p_type != PT_NOTE || phdr.p_filesz == 0) {
            continue;
        }
        const std::uintptr_t address =
            image.base + static_cast<std::uintptr_t>(phdr.p_vaddr);
        if (!aot::ContainsAddress(image, address,
                                  static_cast<std::size_t>(phdr.p_filesz))) {
            continue;
        }

        const std::uint8_t* cursor =
            reinterpret_cast<const std::uint8_t*>(address);
        std::size_t remaining = static_cast<std::size_t>(phdr.p_filesz);
        while (remaining >= sizeof(ElfW(Nhdr))) {
            ElfW(Nhdr) note = {};
            std::memcpy(&note, cursor, sizeof(note));
            const std::size_t name_size =
                (static_cast<std::size_t>(note.n_namesz) + 3u) & ~3u;
            const std::size_t desc_size =
                (static_cast<std::size_t>(note.n_descsz) + 3u) & ~3u;
            if (sizeof(note) + name_size + desc_size > remaining) {
                break;
            }
            const std::uint8_t* note_name = cursor + sizeof(note);
            const std::uint8_t* note_desc = note_name + name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(note_name, "GNU", 4) == 0) {
                char build_id[160] = {};
                std::size_t offset = 0;
                AppendHex(build_id, sizeof(build_id), &offset, note_desc,
                          note.n_descsz);
                Writef(ANDROID_LOG_INFO, "diag: %s build-id=%s", label,
                       build_id);
            }
            const std::size_t step =
                sizeof(note) + name_size + desc_size;
            cursor += step;
            remaining -= step;
        }
    }
}


namespace {

struct Anchor {
    const char* name;
    const char* text;
};

// Human-visible strings that sit next to the five features. They are not hook
// targets themselves; they are landmarks for identifying the containing Dart
// function and the object-pool slot that holds the literal on this build.
constexpr Anchor kAnchors[] = {
    {"drawer-all-tab", "全部"},
    {"drawer-category", "分类"},
    {"edit-button", "编辑"},
    {"settings-gear", "设置"},
    {"search-box", "搜索"},
    {"swipe-up-arrow", "上滑"},
};

std::vector<aot::Span> ReadableSegments(const aot::Image& image,
                                        bool include_executable) {
    std::vector<aot::Span> spans;
    if (!image.Valid()) {
        return spans;
    }
    for (std::size_t i = 0; i < image.phnum; ++i) {
        const ElfW(Phdr)& phdr = image.phdrs[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_R) == 0 ||
            phdr.p_filesz == 0) {
            continue;
        }
        if (!include_executable && (phdr.p_flags & PF_X) != 0) {
            continue;
        }
        aot::Span span{};
        span.address = image.base + static_cast<std::uintptr_t>(phdr.p_vaddr);
        span.data = reinterpret_cast<const std::uint8_t*>(span.address);
        span.size = static_cast<std::size_t>(phdr.p_filesz);
        spans.push_back(span);
    }
    return spans;
}

std::vector<std::uintptr_t> FindBytes(const aot::Span& span,
                                      const std::uint8_t* needle,
                                      std::size_t needle_size,
                                      std::size_t max_hits) {
    std::vector<std::uintptr_t> hits;
    if (!span.Valid() || needle == nullptr || needle_size == 0 ||
        needle_size > span.size) {
        return hits;
    }

    const std::uint8_t* const end = span.data + span.size - needle_size + 1;
    const std::uint8_t* cursor = span.data;
    while (cursor < end && hits.size() < max_hits) {
        const std::size_t available = static_cast<std::size_t>(end - cursor);
        const void* found = std::memchr(cursor, needle[0], available);
        if (found == nullptr) {
            break;
        }
        const auto* candidate = static_cast<const std::uint8_t*>(found);
        if (std::memcmp(candidate, needle, needle_size) == 0) {
            hits.push_back(span.address +
                           static_cast<std::uintptr_t>(candidate - span.data));
        }
        cursor = candidate + 1;
    }
    return hits;
}

std::vector<std::uint8_t> Utf8ToUtf16Le(std::string_view text) {
    std::vector<std::uint8_t> output;
    output.reserve(text.size() * 2);
    const auto append = [&output](std::uint32_t code_unit) {
        output.push_back(static_cast<std::uint8_t>(code_unit & 0xFFu));
        output.push_back(static_cast<std::uint8_t>((code_unit >> 8) & 0xFFu));
    };

    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::uint32_t code_point = 0;
        std::size_t extra = 0;
        if (first < 0x80) {
            code_point = first;
        } else if ((first & 0xE0) == 0xC0) {
            code_point = first & 0x1Fu;
            extra = 1;
        } else if ((first & 0xF0) == 0xE0) {
            code_point = first & 0x0Fu;
            extra = 2;
        } else if ((first & 0xF8) == 0xF0) {
            code_point = first & 0x07u;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= text.size()) {
            break;
        }
        bool valid = true;
        for (std::size_t j = 1; j <= extra; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code_point = (code_point << 6) | (next & 0x3Fu);
        }
        i += extra + 1;
        if (!valid) {
            continue;
        }
        if (code_point <= 0xFFFFu) {
            append(code_point);
        } else if (code_point <= 0x10FFFFu) {
            code_point -= 0x10000u;
            append(0xD800u | (code_point >> 10));
            append(0xDC00u | (code_point & 0x3FFu));
        }
    }
    return output;
}


void ProbeHit(const aot::Image& image, const std::vector<aot::Span>& spans,
              const Anchor& anchor, const char* encoding,
              std::uintptr_t hit, std::size_t units) {
    const std::uintptr_t offset =
        hit >= image.base ? hit - image.base : 0;
    Writef(ANDROID_LOG_INFO,
           "diag: probe %s/%s hit=%p libapp+0x%zx units=%zu", anchor.name,
           encoding, reinterpret_cast<void*>(hit), offset, units);

    constexpr std::uintptr_t kStringDataOffset = 16;
    if (hit < image.base + kStringDataOffset) {
        return;
    }
    const std::uintptr_t raw = (hit - kStringDataOffset) & ~std::uintptr_t{1};
    if ((raw & 7u) != 0 || !aot::ContainsAddress(image, raw, 16)) {
        Writef(ANDROID_LOG_WARN,
               "diag: probe %s/%s: no plausible object header at %p",
               anchor.name, encoding, reinterpret_cast<void*>(raw));
        return;
    }

    std::uint32_t tags = 0;
    std::uint32_t length_at_8 = 0;
    std::uint32_t length_at_12 = 0;
    std::memcpy(&tags, reinterpret_cast<const void*>(raw), sizeof(tags));
    std::memcpy(&length_at_8, reinterpret_cast<const void*>(raw + 8),
                sizeof(length_at_8));
    std::memcpy(&length_at_12, reinterpret_cast<const void*>(raw + 12),
                sizeof(length_at_12));
    const std::uint32_t class_id = (tags >> 12) & 0xFFFFFu;
    const auto length_matches = [units](std::uint32_t length) {
        return length == units || length == (units << 1) ||
               (length >> 1) == units;
    };
    const bool length_at_8_matches = length_matches(length_at_8);
    const bool length_at_12_matches = length_matches(length_at_12);
    const bool length_matches_any =
        length_at_8_matches || length_at_12_matches;
    const std::uintptr_t tagged = raw | 1;
    Writef(ANDROID_LOG_INFO,
           "diag: probe %s/%s object raw=%p class=%u length8=%u(%d) "
           "length12=%u(%d) tagged=%p",
           anchor.name, encoding, reinterpret_cast<void*>(raw), class_id,
           length_at_8, static_cast<int>(length_at_8_matches), length_at_12,
           static_cast<int>(length_at_12_matches),
           reinterpret_cast<void*>(tagged));

    if (class_id == 0 || !length_matches_any) {
        Writef(ANDROID_LOG_WARN,
               "diag: probe %s/%s: header is not a trustworthy string object; "
               "pool refs skipped",
               anchor.name, encoding);
        return;
    }

    std::size_t refs = 0;
    for (const aot::Span& span : spans) {
        for (std::size_t pos = 0; pos + sizeof(std::uint64_t) <= span.size;
             pos += sizeof(std::uint64_t)) {
            std::uint64_t value = 0;
            std::memcpy(&value, span.data + pos, sizeof(value));
            if (value != tagged) {
                continue;
            }
            const std::uintptr_t address =
                span.address + static_cast<std::uintptr_t>(pos);
            Writef(ANDROID_LOG_INFO,
                   "diag: probe %s/%s pool-like ref=%p libapp+0x%zx",
                   anchor.name, encoding, reinterpret_cast<void*>(address),
                   address >= image.base ? address - image.base : 0);
            if (++refs >= 8) {
                break;
            }
        }
        if (refs >= 8) {
            break;
        }
    }
    if (refs == 0) {
        Writef(ANDROID_LOG_WARN,
               "diag: probe %s/%s: no direct tagged-pointer reference found",
               anchor.name, encoding);
    }
}


}  // namespace

void ProbeDartImage(const aot::Image& image) {
    if (!image.Valid()) {
        Writef(ANDROID_LOG_WARN, "diag: Dart probe skipped: invalid image");
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<aot::Span> spans = ReadableSegments(image, false);
    if (spans.empty()) {
        Writef(ANDROID_LOG_WARN,
               "diag: Dart probe: no non-executable readable segment; "
               "scanning every readable segment");
        spans = ReadableSegments(image, true);
    }
    if (spans.empty()) {
        Writef(ANDROID_LOG_WARN,
               "diag: Dart probe skipped: no readable PT_LOAD segment");
        return;
    }

    Writef(ANDROID_LOG_INFO,
           "diag: Dart probe: spans=%zu anchors=%zu; results identify the "
           "build and candidate pool slots, not yet hook targets",
           spans.size(), sizeof(kAnchors) / sizeof(kAnchors[0]));

    constexpr std::size_t kMaxHits = 4;
    constexpr std::size_t kMaxProbedHits = 2;

    for (const Anchor& anchor : kAnchors) {
        const std::string_view text(anchor.text);
        const auto utf16 = Utf8ToUtf16Le(text);

        struct Encoding {
            const char* name;
            const std::uint8_t* bytes;
            std::size_t size;
            std::size_t units;
        };
        const Encoding encodings[] = {
            {"utf8", reinterpret_cast<const std::uint8_t*>(anchor.text),
             text.size(), text.size()},
            {"utf16le", utf16.data(), utf16.size(), utf16.size() / 2},
        };

        for (const Encoding& encoding : encodings) {
            std::vector<std::uintptr_t> hits;
            for (const aot::Span& span : spans) {
                const std::vector<std::uintptr_t> found =
                    FindBytes(span, encoding.bytes, encoding.size,
                              kMaxHits - hits.size());
                hits.insert(hits.end(), found.begin(), found.end());
                if (hits.size() >= kMaxHits) {
                    break;
                }
            }
            if (hits.empty()) {
                Writef(ANDROID_LOG_WARN, "diag: probe %s/%s: not found",
                       anchor.name, encoding.name);
                continue;
            }
            Writef(ANDROID_LOG_INFO, "diag: probe %s/%s: %zu hit(s)",
                   anchor.name, encoding.name, hits.size());
            const std::size_t probe_count =
                hits.size() < kMaxProbedHits ? hits.size() : kMaxProbedHits;
            for (std::size_t i = 0; i < probe_count; ++i) {
                ProbeHit(image, spans, anchor, encoding.name, hits[i],
                         encoding.units);
            }
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    Writef(ANDROID_LOG_INFO, "diag: Dart probe finished in %lld ms",
           static_cast<long long>(elapsed));
}

}  // namespace miuihome::diag

