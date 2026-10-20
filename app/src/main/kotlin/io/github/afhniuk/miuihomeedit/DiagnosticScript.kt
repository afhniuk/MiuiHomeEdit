package io.github.afhniuk.miuihomeedit

/**
 * Builds the POSIX sh script used by [ConfigStore.collectDiagnostics].
 *
 * The script is written to a file and executed with `su -c "sh <file>"`.
 * Passing a multi-kilobyte script as one su argument is fragile on some su
 * builds: the argument can be truncated, which makes the shell report
 * `/system/bin/sh: no closing quote` and hides every section after the cut.
 *
 * The template is a raw Kotlin string, so every backslash is intended for the
 * shell literally. `§` stands for a dollar sign; it is replaced after the
 * template has been assembled, which avoids Kotlin interpolation in shell
 * variables such as `$P_DIR` and `$(getprop ...)`.
 */
internal object DiagnosticScript {
    fun build(
        modulePackage: String,
        targetPackage: String,
        configFile: String,
        privateDir: String,
        deviceDir: String,
        externalDir: String,
        publicDir: String,
        userId: Int,
    ): String {
        val script = """
P_DIR={{P_DIR}}
D_DIR={{D_DIR}}
E_DIR={{E_DIR}}
PUB_DIR={{PUB_DIR}}
CONFIG={{CONFIG}}

echo '===== script ====='
echo "P_DIR=§P_DIR"
echo "E_DIR=§E_DIR"
echo "PUB_DIR=§PUB_DIR"

echo '===== identity ====='
id
date
uname -a

echo '===== properties ====='
for k in ro.build.version.release ro.build.version.sdk \
  ro.build.version.security_patch ro.build.fingerprint \
  ro.build.version.incremental ro.build.type ro.build.tags \
  ro.debuggable ro.secure ro.product.manufacturer \
  ro.product.model ro.product.device ro.product.cpu.abilist \
  ro.mi.os.version.name ro.mi.os.version.code \
  ro.miui.ui.version.name ro.miui.ui.version.code \
  sys.boot_completed persist.sys.timezone; do
  echo "§k=§(getprop §k)"
done

echo '===== packages ====='
for pkg in {{MODULE_Q}} {{TARGET_Q}}; do
  echo "--- §pkg"
  dumpsys package "§pkg" 2>&1 | grep -E \
    'versionName=|versionCode=|codePath=|primaryCpuAbi=|enabled=|stopped=|hidden=|suspended=|installed=' \
    | head -n 80
done
pm path {{MODULE_Q}} 2>&1
pm path {{TARGET_Q}} 2>&1
echo '===== launcher processes ====='
PIDS=''
for d in /proc/[0-9]*; do
  [ -r "§d/cmdline" ] || continue
  cmd=§(tr '\000' '\n' < "§d/cmdline" 2>/dev/null | head -n 1)
  case "§cmd" in
    {{TARGET}}|{{TARGET}}:*) PIDS="§PIDS §{d#/proc/}" ;;
  esac
done
echo "pids=§PIDS"
for pid in §PIDS; do
  echo "--- /proc/§pid/cmdline"
  tr '\000' ' ' < "/proc/§pid/cmdline" 2>&1; echo
  echo "--- /proc/§pid/status"
  grep -E '^(Name|Pid|PPid|Uid|Gid|State|Threads):' \
    "/proc/§pid/status" 2>&1 || echo '(no matching status fields)'
  echo "--- /proc/§pid/exe"
  readlink "/proc/§pid/exe" 2>&1
  echo "--- /proc/§pid/maps (libapp/libflutter/module/apk)"
  grep -E 'libapp|libflutter|libmiuihome_edit|libdobby|\.apk' \
    "/proc/§pid/maps" 2>&1 | head -n 200 || echo '(no matching mapping)'
  # An .so stored inside an APK is mapped as [apk path + file offset], so its
  # soname never appears in maps. A line saying the library is absent therefore
  # only means it is not a separate file; the authoritative list is the native
  # log image: and hooked <name> lines.
  echo "maps note: APK-embedded libs appear as their .apk path, not by soname"
  echo "--- /proc/§pid/maps head"
  head -n 30 "/proc/§pid/maps" 2>&1
  echo "--- /proc/§pid/fd (apk/lib)"
  ls -l "/proc/§pid/fd" 2>&1 | grep -E '\.apk|libapp|libflutter|miuihome' \
    | head -n 40 || echo '(no matching fd)'
done

echo '===== config candidates ====='
for f in "§P_DIR/§CONFIG" "§D_DIR/§CONFIG" "§E_DIR/§CONFIG" "§PUB_DIR/§CONFIG" "/data/local/tmp/§CONFIG"; do
  echo "--- §f"
  if [ ! -e "§f" ]; then echo '(missing)'; continue; fi
  ls -ln "§f" 2>&1
  ls -lZ "§f" 2>&1
  stat -c 'mode=%a uid=%u gid=%g size=%s mtime=%y' "§f" 2>&1
  head -c 8192 "§f" 2>&1
  echo
done

echo '===== native logs ====='
LOG_FOUND=0
for d in §P_DIR §D_DIR §E_DIR §PUB_DIR /data/local/tmp; do
  echo "--- §d"
  ls -laZ "§d" 2>&1 | head -n 40
done
for f in "§P_DIR"/miuihome_edit.log* "§D_DIR"/miuihome_edit.log* "§E_DIR"/miuihome_edit.log* \
         "§PUB_DIR"/miuihome_edit.log* /data/local/tmp/miuihome_edit.log*; do
  [ -f "§f" ] || continue
  LOG_FOUND=1
  echo "--- §f"
  ls -ln "§f" 2>&1
  ls -lZ "§f" 2>&1
  tail -n 1500 "§f" 2>&1
done
# On HyperOS 4 + KernelSU the su domain is denied the private app_data_file
# directory of the launcher even though the file exists. The mount namespace of
# the launcher process reaches it, so retry through /proc/<pid>/root before
# reporting a missing log.
for pid in §PIDS; do
  for f in "/proc/§pid/root§P_DIR"/miuihome_edit.log*; do
    [ -f "§f" ] || continue
    LOG_FOUND=1
    echo "--- §f (via /proc/§pid/root)"
    ls -ln "§f" 2>&1
    tail -n 1500 "§f" 2>&1
  done
done
[ "§LOG_FOUND" = 1 ] || echo '(no native log found)'

echo '===== logcat ====='
logcat -d -v threadtime -t 6000 -s \
  'MiuiHomeEdit:*' 'LSPosed:*' 'LSPosed-Bridge:*' 'Vector:*' \
  'libxposed:*' 'Xposed:*' 'AndroidRuntime:E' 'libc:F' 'DEBUG:E' 2>&1

echo '===== logcat broad ====='
logcat -d -v threadtime -t 12000 2>&1 | grep -E \
  'MiuiHome|miuihome|LSPosed|Vector|libxposed|Xposed|UnsatisfiedLink|dlopen|com\.miui\.home' \
  | tail -n 3000

echo '===== logcat crash ====='
logcat -d -b crash -v threadtime -t 800 2>&1

echo '===== framework files ====='
for d in /data/adb/lspd /data/adb/vector /data/adb/modules /data/adb/ksu; do
  echo "--- §d"
  ls -la "§d" 2>&1 | head -n 80
  ls -laZ "§d" 2>&1 | head -n 80
done

echo '===== framework packages ====='
pm list packages 2>&1 | grep -Ei 'lsposed|vector|xposed' | head -n 40

echo '===== lsposed database ====='
if command -v sqlite3 >/dev/null 2>&1; then
  sqlite3 /data/adb/lspd/config/modules_config.db '.tables' 2>&1
  sqlite3 /data/adb/lspd/config/modules_config.db 'select * from modules;' 2>&1
  sqlite3 /data/adb/lspd/config/modules_config.db "select m.module_pkg_name, m.apk_path, s.app_pkg_name, s.user_id from modules m left join scope s on s.mid = m.mid where m.module_pkg_name = {{MODULE_Q}};" 2>&1
  sqlite3 /data/adb/lspd/config/modules_config.db "select * from scope where app_pkg_name = {{TARGET_Q}};" 2>&1

else
  echo '(sqlite3 unavailable)'
fi

echo '===== framework logs ====='
FW_FOUND=0
for f in /data/adb/lspd/log/* /data/adb/lspd/log/*/* /data/adb/lspd/log*/* /data/adb/vector/log/* /data/adb/vector/log/*/*; do
  [ -f "§f" ] || continue
  FW_FOUND=1
  echo "--- §f"
  tail -n 500 "§f" 2>&1
done
[ "§FW_FOUND" = 1 ] || echo '(no framework log file found)'

echo '===== uid/user ====='
echo "requested_user={{USER}}"
echo '===== done ====='
""".trimIndent().replace("\r\n", "\n")

        return script
            .replace("§", "$")
            .replace("{{P_DIR}}", shellQuote(privateDir))
            .replace("{{D_DIR}}", shellQuote(deviceDir))
            .replace("{{E_DIR}}", shellQuote(externalDir))
            .replace("{{PUB_DIR}}", shellQuote(publicDir))
            .replace("{{MODULE_Q}}", shellQuote(modulePackage))
            .replace("{{TARGET_Q}}", shellQuote(targetPackage))
            .replace("{{TARGET}}", targetPackage)
            .replace("{{CONFIG}}", configFile)
            .replace("{{USER}}", userId.toString())
    }

    private fun shellQuote(value: String): String =
        "'" + value.replace("'", "'\\''") + "'"
}

