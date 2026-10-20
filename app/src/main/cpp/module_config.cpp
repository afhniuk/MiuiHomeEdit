/**
 * module_config.cpp - reads the control-panel JSON without a JSON dependency.
 *
 * The document is flat and tiny:
 *
 *     {
 *       "version": 1,
 *       "hide_drawer_all_tab": false,
 *       "hide_edit_button": false,
 *       "hide_settings_gear": false,
 *       "hide_search_box": false,
 *       "hide_swipe_up_arrow": false
 *     }
 *
 * The parser understands all JSON value types so that unknown keys written by
 * a future version of the app are skipped instead of rejecting the file, but
 * it only publishes the five known booleans. It is deliberately exception-free
 * (the module is compiled with -fno-exceptions) and bounded in file size and
 * nesting depth.
 */

#include "module_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "log.h"

namespace miuihome::config {
namespace {

constexpr std::size_t kMaxConfigBytes = 64u * 1024u;
constexpr int kMaxJsonDepth = 8;
constexpr auto kReloadInterval = std::chrono::seconds(2);

constexpr std::uint32_t Bit(Feature feature) noexcept {
    return static_cast<std::uint32_t>(feature);
}

std::atomic<std::uint32_t> g_bits{0};
std::atomic<bool> g_initialized{false};
std::atomic<std::int64_t> g_next_check_ns{0};
std::atomic<std::uint64_t> g_signature{0};
std::mutex g_mutex;

// Guarded by g_mutex. Used to avoid logging the same successful source and
// values on every reload.
std::string g_source;

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AddPath(std::vector<std::string> *paths, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

void AddPath(std::vector<std::string> *paths, const char *format, ...) {
    char buffer[512] = {};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written > 0 && static_cast<std::size_t>(written) < sizeof(buffer)) {
        paths->emplace_back(buffer);
    }
}

/**
 * Candidate paths. When several files are readable, the most recently written
 * one wins; this order only breaks mtime ties in favour of the reliable paths.
 *
 *   1. com.miui.home's device-encrypted files directory. This is the only
 *      location guaranteed to be readable while the user is still locked
 *      (Direct Boot): CE storage and /sdcard are often not mounted yet when the
 *      launcher process starts at boot. The control panel writes it through
 *      root, alongside the CE copy.
 *   2. com.miui.home's own CE files directory. The launcher process always has
 *      access after unlock, and the control panel writes it through root.
 *      /data/user/<user> is correct for work profiles too.
 *   3. The launcher's external files directory. Also readable by the launcher
 *      without storage permissions; a useful fallback when the private write
 *      failed but a root copy to /sdcard succeeded.
 *   4. A public directory, for users who granted "All files access" instead of
 *      root. com.miui.home may not be allowed to read it, so it is last.
 *   5. /data/local/tmp, only as a debugging convenience; SELinux normally
 *      denies app access to shell_data_file, so this is best-effort.
 */
std::vector<std::string> CandidatePaths() {
    std::vector<std::string> paths;
    const uid_t uid = getuid();
    const int user = static_cast<int>(uid / 100000);

    AddPath(&paths, "/data/user_de/%d/%s/files/miuihome_edit.json", user,
            "com.miui.home");
    AddPath(&paths, "/data/user/%d/%s/files/miuihome_edit.json", user,
            "com.miui.home");
    AddPath(&paths,
            "/storage/emulated/%d/Android/data/com.miui.home/files/"
            "miuihome_edit.json",
            user);
    AddPath(&paths, "/storage/emulated/%d/MiuiHomeEdit/miuihome_edit.json",
            user);
    paths.emplace_back("/data/local/tmp/miuihome_edit.json");
    return paths;
}

/**
 * Reads a regular file without following a symlink at the final component.
 * On success, mtime_ns receives the modification time in nanoseconds so the
 * caller can prefer the most recently written candidate.
 */
bool ReadFile(const std::string &path, std::string *text,
              std::int64_t *mtime_ns) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        const int error = errno;
        LOGW("config: open %s failed: %s (errno=%d)", path.c_str(),
             std::strerror(error), error);
        return false;
    }

    struct stat info = {};
    bool ok = false;
    if (fstat(fd, &info) != 0) {
        const int error = errno;
        LOGW("config: fstat %s failed: %s (errno=%d)", path.c_str(),
             std::strerror(error), error);
    } else if (!S_ISREG(info.st_mode)) {
        LOGW("config: %s is not a regular file", path.c_str());
    } else if (info.st_size <= 0) {
        LOGW("config: %s is empty", path.c_str());
    } else if (static_cast<std::uint64_t>(info.st_size) > kMaxConfigBytes) {
        LOGW("config: %s is too large (%lld bytes)", path.c_str(),
             static_cast<long long>(info.st_size));
    } else {
        text->assign(static_cast<std::size_t>(info.st_size), '\0');
        std::size_t offset = 0;
        while (offset < text->size()) {
            const ssize_t count =
                read(fd, text->data() + offset, text->size() - offset);
            if (count <= 0) {
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
        ok = offset == text->size();
        if (ok) {
            *mtime_ns =
                static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000000000LL +
                static_cast<std::int64_t>(info.st_mtim.tv_nsec);
        } else {
            LOGW("config: short read on %s (%zu/%zu bytes)", path.c_str(),
                 offset, text->size());
        }
    }
    close(fd);
    return ok;
}

std::uint32_t BitForKey(std::string_view key) {
    if (key == "hide_drawer_all_tab") {
        return Bit(Feature::kHideDrawerAllTab);
    }
    if (key == "hide_edit_button") {
        return Bit(Feature::kHideEditButton);
    }
    if (key == "hide_settings_gear") {
        return Bit(Feature::kHideSettingsGear);
    }
    if (key == "hide_search_box") {
        return Bit(Feature::kHideSearchBox);
    }
    if (key == "hide_swipe_up_arrow") {
        return Bit(Feature::kHideSwipeUpArrow);
    }
    return 0;
}

/**
 * Minimal, bounded JSON reader. It is intentionally not a general-purpose
 * library: no exceptions, no DOM, no comments. All unknown values are skipped,
 * so adding a key in a later app version does not invalidate the file.
 */
class JsonParser {
  public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool Parse(std::uint32_t *bits) {
        SkipBom();
        SkipWhitespace();
        if (!Consume('{')) {
            return false;
        }
        if (!ParseObjectBody(bits)) {
            return false;
        }
        SkipWhitespace();
        return AtEnd();
    }

  private:
    bool ParseObjectBody(std::uint32_t *bits) {
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            std::string key;
            if (!ParseString(&key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();

            const std::uint32_t bit = BitForKey(key);
            if (bit != 0) {
                bool enabled = false;
                if (!ParseBool(&enabled)) {
                    return false;
                }
                if (enabled) {
                    *bits |= bit;
                } else {
                    *bits &= ~bit;
                }
            } else if (!SkipValue(0)) {
                return false;
            }

            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseBool(bool *value) {
        if (MatchLiteral("true")) {
            *value = true;
            return true;
        }
        if (MatchLiteral("false")) {
            *value = false;
            return true;
        }
        return false;
    }

    bool ParseString(std::string *out) {
        if (!Consume('"')) {
            return false;
        }
        out->clear();
        while (!AtEnd()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (AtEnd()) {
                    return false;
                }
                const char escaped = text_[pos_++];
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/':
                        out->push_back(escaped);
                        break;
                    case 'b':
                        out->push_back('\b');
                        break;
                    case 'f':
                        out->push_back('\f');
                        break;
                    case 'n':
                        out->push_back('\n');
                        break;
                    case 'r':
                        out->push_back('\r');
                        break;
                    case 't':
                        out->push_back('\t');
                        break;
                    case 'u':
                        // The keys emitted by the control panel are ASCII. A
                        // unicode escape cannot match one of them; consume it
                        // so an unknown key does not make the whole file fail.
                        if (pos_ + 4 > text_.size()) {
                            return false;
                        }
                        pos_ += 4;
                        out->push_back('?');
                        break;
                    default:
                        return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            } else {
                out->push_back(c);
            }
        }
        return false;
    }

    bool SkipValue(int depth) {
        if (depth > kMaxJsonDepth || AtEnd()) {
            return false;
        }
        switch (text_[pos_]) {
            case '"':
                return SkipString();
            case '{':
                return SkipObject(depth + 1);
            case '[':
                return SkipArray(depth + 1);
            case 't':
                return MatchLiteral("true");
            case 'f':
                return MatchLiteral("false");
            case 'n':
                return MatchLiteral("null");
            default:
                return SkipNumber();
        }
    }

    bool SkipObject(int depth) {
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }
        for (;;) {
            if (!SkipString()) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }
            SkipWhitespace();
            if (!SkipValue(depth)) {
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool SkipArray(int depth) {
        if (!Consume('[')) {
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        for (;;) {
            if (!SkipValue(depth)) {
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool SkipString() {
        if (!Consume('"')) {
            return false;
        }
        while (!AtEnd()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (AtEnd()) {
                    return false;
                }
                const char escaped = text_[pos_++];
                if (escaped == 'u') {
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (!IsHex(text_[pos_ + static_cast<std::size_t>(i)])) {
                            return false;
                        }
                    }
                    pos_ += 4;
                } else if (std::strchr("\"\\/bfnrt", escaped) == nullptr) {
                    return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
        }
        return false;
    }

    bool SkipNumber() {
        const std::size_t start = pos_;
        Consume('-');
        bool digits = false;
        while (!AtEnd() && IsDigit(text_[pos_])) {
            ++pos_;
            digits = true;
        }
        if (Consume('.')) {
            bool fraction = false;
            while (!AtEnd() && IsDigit(text_[pos_])) {
                ++pos_;
                fraction = true;
            }
            if (!fraction) {
                return false;
            }
        }
        if (!AtEnd() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (!AtEnd() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            bool exponent = false;
            while (!AtEnd() && IsDigit(text_[pos_])) {
                ++pos_;
                exponent = true;
            }
            if (!exponent) {
                return false;
            }
        }
        return digits && pos_ > start;
    }

    bool MatchLiteral(std::string_view literal) {
        if (text_.size() - pos_ < literal.size()) {
            return false;
        }
        if (text_.substr(pos_, literal.size()) != literal) {
            return false;
        }
        pos_ += literal.size();
        return true;
    }

    bool Consume(char expected) {
        if (!AtEnd() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                return;
            }
            ++pos_;
        }
    }

    void SkipBom() {
        if (text_.size() >= 3 &&
            static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    bool AtEnd() const { return pos_ >= text_.size(); }

    static bool IsDigit(char c) { return c >= '0' && c <= '9'; }
    static bool IsHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

bool ParseConfig(std::string_view text, std::uint32_t *bits) {
    *bits = 0;
    JsonParser parser(text);
    return parser.Parse(bits);
}

std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    return hash;
}

/**
 * Changes whenever a candidate file is created, removed or modified. mtime is
 * included at nanosecond resolution because the control panel may replace two
 * equally sized documents within the same second.
 */
std::uint64_t ComputeSignature(const std::vector<std::string> &paths) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::string &path : paths) {
        struct stat info = {};
        if (stat(path.c_str(), &info) == 0) {
            hash = HashValue(hash, 1);
            hash = HashValue(
                hash, static_cast<std::uint64_t>(info.st_mtim.tv_sec));
            hash = HashValue(
                hash, static_cast<std::uint64_t>(info.st_mtim.tv_nsec));
            hash = HashValue(hash, static_cast<std::uint64_t>(info.st_size));
            hash = HashValue(hash, static_cast<std::uint64_t>(info.st_ino));
        } else {
            hash = HashValue(hash, 0);
        }
    }
    return hash;
}

void LogBits(std::uint32_t bits) {
    LOGI("config: drawer-all-tab=%d edit-button=%d settings-gear=%d "
         "search-box=%d swipe-up-arrow=%d",
         static_cast<int>((bits & Bit(Feature::kHideDrawerAllTab)) != 0),
         static_cast<int>((bits & Bit(Feature::kHideEditButton)) != 0),
         static_cast<int>((bits & Bit(Feature::kHideSettingsGear)) != 0),
         static_cast<int>((bits & Bit(Feature::kHideSearchBox)) != 0),
         static_cast<int>((bits & Bit(Feature::kHideSwipeUpArrow)) != 0));
}

/**
 * Logs why each candidate is or is not readable before the first parse. This
 * distinguishes the two common "switches do nothing" reports:
 *
 *   - the root write landed somewhere else (stat fails with ENOENT), and
 *   - the file exists but this process cannot open it (SELinux/permissions).
 */
void LogCandidateStatus(const std::vector<std::string> &paths) {
    for (const std::string &path : paths) {
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0) {
            const int error = errno;
            LOGW("config: candidate %s: stat failed: %s (errno=%d)",
                 path.c_str(), std::strerror(error), error);
            continue;
        }

        errno = 0;
        const int readable = access(path.c_str(), R_OK);
        const int error = errno;
        LOGI("config: candidate %s: mode=%o uid=%u gid=%u size=%lld "
             "mtime=%lld.%09ld readable=%d%s",
             path.c_str(), static_cast<unsigned>(info.st_mode),
             static_cast<unsigned>(info.st_uid),
             static_cast<unsigned>(info.st_gid),
             static_cast<long long>(info.st_size),
             static_cast<long long>(info.st_mtim.tv_sec),
             static_cast<long>(info.st_mtim.tv_nsec), readable == 0 ? 1 : 0,
             readable == 0 ? "" : " (access denied)");
        if (readable != 0) {
            LOGW("config: candidate %s: access(R_OK) failed: %s (errno=%d)",
                 path.c_str(), std::strerror(error), error);
        }
    }
}

/**
 * Writes the currently selected JSON into com.miui.home's device-encrypted
 * files directory. The launcher process itself runs with the app UID, so this
 * works even on devices where the Root/KernelSU domain cannot see or write the
 * launcher's private app_data_file directories. Next boot can then read the
 * switches before CE storage and /sdcard are mounted.
 */
bool PersistDeviceEncrypted(const std::vector<std::string> &paths,
                            const std::string &source,
                            const std::string &text) {
    if (paths.empty() || text.empty() || source == paths.front()) {
        return false;
    }
    const std::string &path = paths.front();
    if (path.rfind("/data/user_de/", 0) != 0) {
        return false;
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return false;
    }
    const std::string directory = path.substr(0, slash);
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        LOGW("config: mkdir %s failed: %s (errno=%d)", directory.c_str(),
             std::strerror(errno), errno);
        return false;
    }

    const std::string temporary = path + ".tmp";
    const int fd = open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                        0644);
    if (fd < 0) {
        LOGW("config: open %s failed: %s (errno=%d)", temporary.c_str(),
             std::strerror(errno), errno);
        return false;
    }
    std::size_t offset = 0;
    bool ok = true;
    while (offset < text.size()) {
        const ssize_t count =
            write(fd, text.data() + offset, text.size() - offset);
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (ok) {
        fsync(fd);
    }
    close(fd);
    if (!ok) {
        unlink(temporary.c_str());
        LOGW("config: short write to %s", temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        LOGW("config: rename %s -> %s failed: %s (errno=%d)",
             temporary.c_str(), path.c_str(), std::strerror(errno), errno);
        unlink(temporary.c_str());
        return false;
    }
    chmod(path.c_str(), 0644);
    LOGI("config: persisted %zu bytes to %s", text.size(), path.c_str());
    return true;
}

/** Called with g_mutex held. */
void LoadLocked(const std::vector<std::string> &paths,
                std::uint64_t signature) {
    std::uint32_t bits = 0;
    std::string source;
    std::string selected_text;
    bool found = false;
    std::int64_t best_mtime_ns = 0;

    // Read every valid candidate and keep the newest one. Fixing the order to
    // "private first, then external" is not enough: if a later write to the
    // private path fails, its old file would shadow the fresh external copy.
    // On identical mtimes the earlier (more reliable) path wins.
    for (const std::string &path : paths) {
        std::string text;
        std::int64_t mtime_ns = 0;
        if (!ReadFile(path, &text, &mtime_ns)) {
            LOGD("config: candidate %s is not a usable regular file",
                 path.c_str());
            continue;
        }
        std::uint32_t parsed = 0;
        if (!ParseConfig(text, &parsed)) {
            LOGW("config: %s is not valid JSON (%zu bytes); ignored",
                 path.c_str(), text.size());
            continue;
        }
        LOGI("config: read %s (%zu bytes, mtime=%lld.%09lld)", path.c_str(),
             text.size(), static_cast<long long>(mtime_ns / 1000000000LL),
             static_cast<long long>(mtime_ns % 1000000000LL));
        if (!found || mtime_ns > best_mtime_ns) {
            bits = parsed;
            source = path;
            selected_text = text;
            best_mtime_ns = mtime_ns;
            found = true;
        }
    }

    const std::uint32_t previous =
        g_bits.exchange(bits, std::memory_order_acq_rel);
    g_signature.store(signature, std::memory_order_release);

    if (!found) {
        if (!g_source.empty() || previous != 0) {
            LOGW("config: no readable config file; all features off");
        }
        g_source.clear();
        return;
    }

    if (!paths.empty() && source != paths.front()) {
        PersistDeviceEncrypted(paths, source, selected_text);
    }

    if (previous != bits || source != g_source) {
        g_source = source;
        LOGI("config: selected %s (bits=0x%08x)", source.c_str(), bits);
        LogBits(bits);
    }
}

}  // namespace

void Initialize() {
    if (g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized.load(std::memory_order_relaxed)) {
        return;
    }

    const std::vector<std::string> paths = CandidatePaths();
    LogCandidateStatus(paths);
    LoadLocked(paths, ComputeSignature(paths));
    g_initialized.store(true, std::memory_order_release);
    g_next_check_ns.store(
        NowNs() +
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kReloadInterval)
                .count(),
        std::memory_order_relaxed);
}

void MaybeReload() {
    if (!g_initialized.load(std::memory_order_acquire)) {
        Initialize();
        return;
    }

    const std::int64_t now = NowNs();
    std::int64_t next = g_next_check_ns.load(std::memory_order_relaxed);
    if (now < next) {
        return;
    }

    const std::int64_t new_next =
        now + std::chrono::duration_cast<std::chrono::nanoseconds>(
                  kReloadInterval)
                  .count();
    if (!g_next_check_ns.compare_exchange_strong(
            next, new_next, std::memory_order_relaxed)) {
        return;
    }

    const std::vector<std::string> paths = CandidatePaths();
    const std::uint64_t signature = ComputeSignature(paths);
    if (signature == g_signature.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (signature != g_signature.load(std::memory_order_relaxed)) {
        LoadLocked(paths, signature);
    }
}

bool IsEnabled(Feature feature) noexcept {
    return (g_bits.load(std::memory_order_acquire) & Bit(feature)) != 0;
}

bool AnyEnabled() noexcept {
    return g_bits.load(std::memory_order_acquire) != 0;
}

}  // namespace miuihome::config
