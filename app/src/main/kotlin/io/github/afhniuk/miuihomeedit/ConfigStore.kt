package io.github.afhniuk.miuihomeedit

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.TimeUnit
import java.util.zip.ZipFile

/**
 * Persists the five switches and copies the document to a path the launcher can
 * read.
 *
 * Android 11+ scoped storage makes another app's Android/data directory
 * unavailable to ordinary File APIs. The reliable route on a rooted device is
 * therefore a short root script that atomically replaces
 * /data/user/<user>/com.miui.home/files/miuihome_edit.json and chowns it to
 * the launcher's UID. The same script also updates the launcher's external
 * files directory. Without root, the panel tries the public folder (and shows
 * that this fallback may not be readable by com.miui.home).
 */
object ConfigStore {
    private const val TAG = "MiuiHomeEdit"
    private const val TARGET_PACKAGE = "com.miui.home"
    private const val CONFIG_FILE = "miuihome_edit.json"
    private const val ROOT_UNAVAILABLE = -2
    private const val ROOT_TIMEOUT_SECONDS = 25L
    private const val DIAGNOSTICS_DIR = "diagnostics"
    private const val DIAGNOSTICS_MAX_OUTPUT = 2 * 1024 * 1024
    private const val DIAGNOSTICS_TIMEOUT_SECONDS = 90L

    data class SaveReport(
        val localOk: Boolean,
        val targetFound: Boolean,
        val rootAvailable: Boolean,
        val privateOk: Boolean,
        val deviceOk: Boolean,
        val externalOk: Boolean,
        val publicOk: Boolean,
        val directExternalOk: Boolean,
        val appExternalOk: Boolean,
        val detail: String,
    ) {
        /** Paths whose access does not depend on the launcher having storage permission. */
        val reliableTargetPath: Boolean
            get() = privateOk || deviceOk || externalOk || directExternalOk

        fun describe(): String = buildString {
            append(
                when {
                    reliableTargetPath -> "配置已写入桌面可读取的路径。\n"
                    publicOk -> "配置已写入公共目录，但桌面可能没有权限读取。\n"
                    else -> "没有写入桌面可读取的路径。\n"
                },
            )
            if (reliableTargetPath) {
                append("重启 com.miui.home 后新配置才会生效；重启前当前桌面进程保持启动时锁定的配置。\n")
            }
            append("Root：").append(if (rootAvailable) "可用" else "不可用或未授权").append('\n')
            append("桌面私有目录：").append(ok(privateOk)).append('\n')
            append("桌面设备加密目录：").append(ok(deviceOk)).append('\n')
            append("桌面共享目录：").append(ok(externalOk)).append('\n')
            append("公共目录：").append(ok(publicOk)).append('\n')
            append("模块本地副本：").append(ok(localOk)).append('\n')
            if (privateOk.not() && (externalOk || directExternalOk)) {
                append("桌面私有目录写入失败，已改用共享目录；桌面会读取最新的有效文件。\n")
            }
            if (!reliableTargetPath) {
                append("请授予 Root 后重试。Android 11+ 上“所有文件访问”通常仍不能写其他应用的 Android/data。\n")
            }
            if (detail.isNotBlank()) {
                append('\n').append(detail.take(700))
            }
            append("\n若桌面已重启仍无效果，请点“收集诊断日志”；报告会区分配置未读到和 hook 目标未配置。")
        }

        private fun ok(value: Boolean): String = if (value) "成功" else "失败"
    }

    data class ShellResult(val exitCode: Int, val output: String, val timedOut: Boolean) {
        val available: Boolean get() = exitCode != ROOT_UNAVAILABLE
    }

    data class DiagnosticReport(
        val path: String,
        val text: String,
        val rootAvailable: Boolean,
        val findings: List<String>,
    ) {
        fun describe(): String = buildString {
            append("诊断报告已生成：\n").append(path).append("\n\n")
            if (findings.isEmpty()) {
                append("没有从日志中识别出明确结论，请导出完整报告后检查。\n")
            } else {
                append("自动结论：\n")
                findings.forEach { findingsLine ->
                    append("• ").append(findingsLine).append('\n')
                }
            }
            append("\n完整日志包含 logcat、native 日志、配置路径和进程映射；")
            append("请点“导出诊断报告”保存后发送。")
        }
    }

    fun loadLocal(context: Context): LauncherConfig {
        val file = File(context.filesDir, CONFIG_FILE)
        if (!file.isFile) {
            return LauncherConfig()
        }
        return try {
            LauncherConfig.fromJson(file.readText(Charsets.UTF_8))
        } catch (e: Exception) {
            Log.w(TAG, "Ignoring unreadable local config", e)
            LauncherConfig()
        }
    }

    fun targetInstalled(context: Context): Boolean = targetUid(context) != null

    fun allFilesAccessGranted(): Boolean = Environment.isExternalStorageManager()

    fun requestAllFilesAccess(context: Context) {
        val appIntent = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${context.packageName}"),
        )
        try {
            context.startActivity(appIntent)
        } catch (e: Exception) {
            Log.w(TAG, "Per-app all-files screen unavailable", e)
            context.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
        }
    }

    // Another app's private data path is deliberately built from /data/user:
    // Context.getFilesDir() only describes this module, not com.miui.home.
    @SuppressLint("SdCardPath")
    fun save(context: Context, config: LauncherConfig): SaveReport {
        val json = config.toJson()
        val localFile = File(context.filesDir, CONFIG_FILE)
        val localOk = writeAtomic(localFile, json)
        if (!localOk) {
            return SaveReport(
                localOk = false,
                targetFound = targetInstalled(context),
                rootAvailable = false,
                privateOk = false,
                deviceOk = false,
                externalOk = false,
                publicOk = false,
                directExternalOk = false,
                appExternalOk = false,
                detail = "无法写入模块私有目录 ${localFile.absolutePath}",
            )
        }

        val uid = targetUid(context)
            ?: return SaveReport(
                localOk = true,
                targetFound = false,
                rootAvailable = false,
                privateOk = false,
                deviceOk = false,
                externalOk = false,
                publicOk = false,
                directExternalOk = false,
                appExternalOk = false,
                detail = "$TARGET_PACKAGE 未安装或不可见（检查 AndroidManifest.xml 的 <queries>）",
            )

        val userId = uid / 100000
        val privateDir = "/data/user/$userId/$TARGET_PACKAGE/files"
        val deviceDir = "/data/user_de/$userId/$TARGET_PACKAGE/files"
        val externalDir = "/storage/emulated/$userId/Android/data/$TARGET_PACKAGE/files"
        val externalFile = File(externalDir, CONFIG_FILE)
        val publicDir = File(Environment.getExternalStorageDirectory(), "MiuiHomeEdit")
        val publicFile = File(publicDir, CONFIG_FILE)
        val appExternalFile = context.getExternalFilesDir(null)?.let { File(it, CONFIG_FILE) }

        val root = runSu(buildRootScript(localFile, privateDir, deviceDir, externalDir, publicDir, uid))
        val privateOk = root.output.contains("PRIVATE_OK")
        val deviceOk = root.output.contains("DEVICE_OK")
        val externalOk = root.output.contains("EXTERNAL_OK")
        val publicRootOk = root.output.contains("PUBLIC_OK")

        val directExternalOk = if (!externalOk) writeAtomic(externalFile, json) else false
        val publicOk = if (publicRootOk) {
            true
        } else if (allFilesAccessGranted()) {
            writeAtomic(publicFile, json)
        } else {
            false
        }
        val appExternalOk = appExternalFile?.let { writeAtomic(it, json) } ?: false

        val report = SaveReport(
            localOk = true,
            targetFound = true,
            // available means the su binary exists; a denied prompt exits non-zero.
            rootAvailable = root.available && root.exitCode == 0,
            privateOk = privateOk,
            deviceOk = deviceOk,
            externalOk = externalOk,
            publicOk = publicOk,
            directExternalOk = directExternalOk,
            appExternalOk = appExternalOk,
            detail = buildDetail(root),
        )
        Log.i(TAG, "save: " + report.describe() + "\nroot output: " + root.output.take(1000))
        return report
    }

    /**
     * Collects the launcher's native log and a filtered logcat snapshot through
     * root, then stores a normal text report in the module's files/diagnostics.
     *
     * The report is intentionally generated on a background thread by the
     * caller: su may wait for the user to approve a prompt, and logcat can be
     * hundreds of kilobytes.
     */
    @SuppressLint("SdCardPath")
    fun collectDiagnostics(context: Context): DiagnosticReport {
        val uid = targetUid(context)
        val userId = (uid ?: 0) / 100000
        val privateDir = "/data/user/$userId/$TARGET_PACKAGE/files"
        val deviceDir = "/data/user_de/$userId/$TARGET_PACKAGE/files"
        val externalDir = "/storage/emulated/$userId/Android/data/$TARGET_PACKAGE/files"
        val publicDir = File(Environment.getExternalStorageDirectory(), "MiuiHomeEdit")
        val localFile = File(context.filesDir, CONFIG_FILE)
        val report = StringBuilder(64 * 1024)

        report.append("MiuiHomeEdit diagnostics\n")
        report.append("generated=").append(timestamp(true)).append('\n')
        report.append("module.package=").append(context.packageName).append('\n')
        report.append("module.version=").append(packageVersion(context, context.packageName))
            .append('\n')
        report.append("target.package=").append(TARGET_PACKAGE).append('\n')
        report.append("target.version=").append(packageVersion(context, TARGET_PACKAGE))
            .append('\n')
        report.append("target.uid=").append(uid?.toString() ?: "<not visible>").append('\n')
        report.append("target.user=").append(userId).append('\n')
        report.append("android.release=").append(Build.VERSION.RELEASE).append('\n')
        report.append("android.sdk=").append(Build.VERSION.SDK_INT).append('\n')
        report.append("android.securityPatch=").append(Build.VERSION.SECURITY_PATCH)
            .append('\n')
        report.append("build.fingerprint=").append(Build.FINGERPRINT).append('\n')
        report.append("build.display=").append(Build.DISPLAY).append('\n')
        report.append("device.model=").append(Build.MODEL).append('\n')
        report.append("device.device=").append(Build.DEVICE).append('\n')
        report.append("abis=").append(Build.SUPPORTED_ABIS.joinToString(",")).append('\n')
        report.append("allFilesAccess=").append(allFilesAccessGranted()).append('\n')
        report.append("privateDir=").append(privateDir).append('\n')
        report.append("deviceDir=").append(deviceDir).append('\n')
        report.append("externalDir=").append(externalDir).append('\n')
        report.append("publicDir=").append(publicDir.absolutePath).append('\n')

        appendTargetApkLibs(report, context)
        appendModuleApkMetadata(report, context)

        appendFileSection(report, "module local config", localFile, 32 * 1024)

        val script = DiagnosticScript.build(
            context.packageName,
            TARGET_PACKAGE,
            CONFIG_FILE,

            privateDir,
            deviceDir,
            externalDir,
            publicDir.absolutePath,
            userId,
        )
        // Passing a multi-kilobyte script as one su argument is fragile: some su
        // builds truncate or re-quote the command, and a truncated quoted string
        // produces the misleading "sh: no closing quote" seen in reports. A
        // short "sh <file>" command avoids argument-length and quoting issues.
        val scriptFiles = mutableListOf(File(context.filesDir, "diagnostics.sh"))
        context.getExternalFilesDir(null)?.let { dir ->
            scriptFiles += File(dir, "diagnostics.sh")
        }
        val writtenScripts = scriptFiles.filter { file ->
            try {
                file.writeText(script, Charsets.UTF_8)
                true
            } catch (e: Exception) {
                Log.w(TAG, "could not write diagnostics script ${file.absolutePath}", e)
                false
            }
        }
        val root = if (writtenScripts.isNotEmpty()) {
            val command = writtenScripts.joinToString(" || ") { file ->
                "sh " + shellQuote(file.absolutePath)
            }
            runSu(command, DIAGNOSTICS_MAX_OUTPUT, DIAGNOSTICS_TIMEOUT_SECONDS)
        } else {
            ShellResult(
                ROOT_UNAVAILABLE,
                "could not write diagnostics.sh to ${scriptFiles.joinToString()}",
                false,
            )
        }
        val primaryScript = writtenScripts.firstOrNull() ?: scriptFiles.first()
        val rootAvailable = root.available && root.exitCode == 0
        report.append("\n===== root collection =====\n")
        report.append("root.available=").append(root.available).append('\n')
        report.append("root.exitCode=").append(root.exitCode).append('\n')
        report.append("root.timedOut=").append(root.timedOut).append('\n')
        report.append("root.timeoutSeconds=").append(DIAGNOSTICS_TIMEOUT_SECONDS).append('\n')
        report.append("root.script=").append(primaryScript.absolutePath).append('\n')
        report.append("root.scriptBytes=").append(script.toByteArray(Charsets.UTF_8).size)
            .append('\n')
        report.append("root.command=").append("su -c \"")
            .append(writtenScripts.joinToString(" || ") { "sh " + shellQuote(it.absolutePath) })
            .append("\"").append('\n')
        if (!root.available) {
            report.append("su unavailable: ").append(root.output.take(500)).append('\n')
        } else if (root.timedOut) {
            report.append("su timed out; approve the root prompt and retry\n")
        } else {
            report.append(root.output)
        }

        // The launcher mirrors its native log into /sdcard/MiuiHomeEdit and into
        // its external files dir. The first copy is readable with all-files
        // access on every Android version; the second one is hidden from that
        // permission on Android 11+ but readable by su. Reading either here
        // works when su is denied the launcher's private app_data_file directory
        // (HyperOS 4 + KernelSU).
        val publicLog = File(publicDir, "miuihome_edit.log")
        val mirrorLog = File(externalDir, "miuihome_edit.log")
        appendNativeLogCopy(report, publicLog)
        appendNativeLogCopy(report, File(publicDir, "miuihome_edit.log.1"))
        appendNativeLogCopy(report, mirrorLog)
        // The rotated copy only exists after the module hit its 2 MiB cap.
        appendNativeLogCopy(report, File(externalDir, "miuihome_edit.log.1"))
        if (!publicLog.exists() && !mirrorLog.exists()) {
            report.append("\n===== native log (direct read) =====\n")
            report.append("path=").append(publicLog.absolutePath).append('\n')
            report.append("path=").append(mirrorLog.absolutePath).append('\n')
            report.append("(not created: the launcher has not mirrored its log yet)\n")
        }

        val text = report.toString()
        val findings = analyzeDiagnostics(text)
        report.append("\n===== automatic findings =====\n")
        if (findings.isEmpty()) {
            report.append("no clear conclusion\n")
        } else {
            findings.forEach { line -> report.append("- ").append(line).append('\n') }
        }

        val directory = File(context.filesDir, DIAGNOSTICS_DIR)
        directory.mkdirs()
        val file = File(directory, "miuihome-edit-${timestamp(false)}.txt")
        file.writeText(report.toString(), Charsets.UTF_8)
        pruneDiagnostics(directory)
        Log.i(TAG, "diagnostics written to ${file.absolutePath}")
        return DiagnosticReport(
            path = file.absolutePath,
            text = report.toString(),
            rootAvailable = rootAvailable,
            findings = findings,
        )
    }


    /**
     * Appends the launcher's native log read directly from this process.
     *
     * diag.cpp mirrors every line into the launcher's external files directory
     * for exactly this case: on HyperOS 4 + KernelSU the su domain is denied
     * /data/user/<user>/com.miui.home/files, so the root script reports
     * "(no native log found)" although the launcher did write one. With
     * all-files access this copy is readable without root.
     */
    @SuppressLint("SdCardPath")
    private fun appendNativeLogCopy(report: StringBuilder, file: File) {
        if (!file.exists()) {
            return
        }
        report.append("\n===== native log (direct read) =====\n")
        report.append("path=").append(file.absolutePath).append('\n')
        val text = try {
            file.readText(Charsets.UTF_8)
        } catch (e: Exception) {
            report.append("read failed: ").append(e.javaClass.simpleName)
                .append(": ").append(e.message).append('\n')
            return
        }
        val lines = text.lineSequence().toList()
        report.append("bytes=").append(text.toByteArray(Charsets.UTF_8).size)
            .append(" lines=").append(lines.size).append('\n')
        // The module caps the file at 2 MiB; keep the newest part only.
        lines.takeLast(1500).forEach { line -> report.append(line).append('\n') }
    }

    fun restartLauncher(context: Context): ShellResult {
        // The root shell runs as user 0; --user is required for a work-profile
        // launcher, otherwise the primary user's process is stopped instead.
        val userId = targetUid(context)?.div(100000) ?: 0
        return runSu("am force-stop --user $userId $TARGET_PACKAGE")
    }
    private fun packageVersion(context: Context, packageName: String): String {
        return try {
            val info = context.packageManager.getPackageInfo(
                packageName,
                PackageManager.PackageInfoFlags.of(0),
            )
            "${info.versionName} (${info.longVersionCode})"
        } catch (e: Exception) {
            "unavailable: ${e.javaClass.simpleName}"
        }
    }

    private fun timestamp(iso: Boolean): String {
        val pattern = if (iso) "yyyy-MM-dd HH:mm:ss Z" else "yyyyMMdd-HHmmss"
        return SimpleDateFormat(pattern, Locale.US).format(Date())
    }

    /**
     * Lists the target APK's native libraries. This runs in the panel process
     * and therefore works even when the root script fails, so it can answer
     * whether the launcher APK contains a Flutter engine and Dart image at all.
     */
    private fun appendTargetApkLibs(report: StringBuilder, context: Context) {
        report.append("\n===== target apk libs =====\n")
        val info = try {
            context.packageManager.getApplicationInfo(
                TARGET_PACKAGE,
                PackageManager.ApplicationInfoFlags.of(0),
            )
        } catch (e: Exception) {
            report.append("application info unavailable: ")
                .append(e.javaClass.simpleName).append(": ").append(e.message).append('\n')
            return
        }
        val apks = mutableListOf(info.sourceDir)
        info.splitSourceDirs?.let { apks.addAll(it) }
        apks.distinct().forEach { path ->
            report.append("apk=").append(path).append('\n')
            try {
                ZipFile(path).use { zip ->
                    val entries = zip.entries().asSequence()
                        .filter { entry ->
                            entry.name.contains("libapp", ignoreCase = true) ||
                                entry.name.contains("libflutter", ignoreCase = true) ||
                                entry.name.startsWith("lib/arm64-v8a/")
                        }
                        .take(80)
                        .toList()
                    if (entries.isEmpty()) {
                        report.append("no libapp/libflutter/arm64 entries\n")
                    } else {
                        entries.forEach { entry ->
                            report.append("  ").append(entry.name)
                                .append(" size=").append(entry.size).append('\n')
                        }
                    }
                }
            } catch (e: Exception) {
                report.append("zip read failed: ").append(e.javaClass.simpleName)
                    .append(": ").append(e.message).append('\n')
            }
        }
    }


    /**
     * Prints the module APK's xposed descriptors and native entries. If the
     * framework never logs MiuiHomeModule, this section proves whether the APK
     * that is actually installed still carries java_init.list/native_init.list.
     */
    private fun appendModuleApkMetadata(report: StringBuilder, context: Context) {
        report.append("\n===== module apk =====\n")
        val apk = context.applicationInfo.sourceDir
        report.append("apk=").append(apk).append('\n')
        try {
            ZipFile(apk).use { zip ->
                val entries = zip.entries().asSequence()
                    .filter { entry ->
                        entry.name.startsWith("META-INF/xposed/") ||
                            entry.name.startsWith("lib/")
                    }
                    .toList()
                if (entries.isEmpty()) {
                    report.append("no META-INF/xposed or lib entries\n")
                }
                entries.forEach { entry ->
                    report.append("entry=").append(entry.name)
                        .append(" size=").append(entry.size).append('\n')
                    if (entry.name.startsWith("META-INF/xposed/") &&
                        entry.size in 1..4096
                    ) {
                        zip.getInputStream(entry).bufferedReader().use { reader ->
                            report.append(reader.readText())
                            if (!report.endsWith("\n")) {
                                report.append('\n')
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            report.append("zip read failed: ").append(e.javaClass.simpleName)
                .append(": ").append(e.message).append('\n')
        }
    }


    private fun appendFileSection(
        builder: StringBuilder,
        title: String,
        file: File,
        maxChars: Int,
    ) {
        builder.append("\n===== ").append(title).append(" =====\n")
        builder.append("path=").append(file.absolutePath).append('\n')
        if (!file.isFile) {
            builder.append("missing\n")
            return
        }
        if (!file.canRead()) {
            builder.append("not readable from the control panel\n")
            return
        }
        try {
            val text = file.readText(Charsets.UTF_8)
            if (text.length > maxChars) {
                builder.append(text.take(maxChars))
                builder.append("\n... truncated at ").append(maxChars).append(" chars\n")
            } else {
                builder.append(text)
            }
            if (!text.endsWith('\n')) {
                builder.append('\n')
            }
        } catch (e: Exception) {
            builder.append("read failed: ").append(e.javaClass.simpleName)
                .append(": ").append(e.message).append('\n')
        }
    }

    private fun pruneDiagnostics(directory: File) {
        directory.listFiles()
            ?.filter { it.isFile }
            ?.sortedByDescending { it.lastModified() }
            ?.drop(5)
            ?.forEach { it.delete() }
    }



    private fun targetUid(context: Context): Int? {
        return try {
            val info = context.packageManager.getApplicationInfo(
                TARGET_PACKAGE,
                PackageManager.ApplicationInfoFlags.of(0),
            )
            info.uid
        } catch (e: PackageManager.NameNotFoundException) {
            null
        }
    }

    /**
     * Small rule set that turns the collected text into the likely cause. It is
     * deliberately conservative: it never claims a hook is active just because
     * a switch is on.
     */
    private fun analyzeDiagnostics(text: String): List<String> {
        val findings = mutableListOf<String>()

        val collectionComplete = text.contains("===== done =====")
        val hasLogSections = text.contains("===== native logs =====") ||
            text.contains("===== logcat =====")

        val hasModuleLoad = text.contains("MiuiHomeModule constructed") ||
            text.contains("module loaded into") ||
            text.contains("loading libmiuihome_edit.so")
        val hasNativeInit = text.contains("native_init: framework API")
        val hasTargetProcess = text.contains("(target)")

        // The launcher process holding the native log open proves that
        // libmiuihome_edit.so was mapped and native_init() ran, independent of
        // how much of the log a given report managed to capture.
        val nativeLogOpen = Regex("l-wx[^\\n]*miuihome_edit\\.log").containsMatchIn(text)
        val directNativeLog = text.contains("===== native log (direct read) =====") &&
            text.contains("lines=")

        if (!collectionComplete) {
            findings += "Root 诊断脚本没有执行到 ===== done =====，报告不完整；" +
                "请安装包含“脚本文件执行”修复的版本后重新收集。"
            if (text.contains("no closing quote") || text.contains("syntax error")) {
                findings += "脚本在 root shell 中被截断或出现引号错误；见 root.script/root.scriptBytes 与 root 输出。"
            }
        }
        if (text.contains("... output truncated at ")) {
            findings += "Root 输出超过报告上限，后面的段可能缺失；可减少 logcat 数量或分段收集。"
        }
        if (hasLogSections) {
            if (!hasModuleLoad && !nativeLogOpen && !directNativeLog) {
                findings += "未看到 MiuiHomeModule 加载日志：模块可能未被框架启用、作用域未包含 " +
                    "$TARGET_PACKAGE，或 APK 被判定为不可用。"
            } else if (!hasNativeInit && (nativeLogOpen || directNativeLog)) {
                findings += "模块确实已注入桌面进程（native 日志已被打开，或已直接读到该日志）；" +
                    "报告里缺少 native_init 行只是采集范围问题，不是加载失败。"
            } else if (!hasNativeInit) {
                findings += "Java 入口已加载，但没有 native_init：libmiuihome_edit.so 未成功映射，" +
                    "请检查 ABI、native_init.list 和 useLegacyPackaging。"
            } else if (!hasTargetProcess) {
                findings += "native_init 已执行，但当前进程不是目标进程；请把作用域设为 $TARGET_PACKAGE，" +
                    "然后强制停止桌面并重新启动。"
            } else {
                findings += "native 模块已注入目标进程（native_init 与 target 标记均存在）。"
            }
        }
        if (text.contains("no target offsets configured")) {
            findings += "根因：dart_hooks.cpp 中所有 hook 目标仍是 kUnset。模块已加载、" +
                "配置也可能已读取，但没有任何代码被 hook，所以开关都不会改变界面。"
        }
        val configBits = Regex("config: (?:snapshot )?selected .* \\(bits=0x([0-9a-fA-F]+)\\)")
            .find(text)
            ?.groupValues
            ?.getOrNull(1)
            ?.toLongOrNull(16)
        if (configBits != null) {
            findings += "桌面进程已成功锁定启动配置快照；若仍有开关无效，问题在 hook 目标，而不是写入路径。"
            if (configBits == 0L) {
                findings += "配置已读到，但五个功能位全部为 0：检查开关写入的 JSON 键名，" +
                    "或保存后是否只更新了旧文件。"
            } else {
                findings += "配置已读到，启用位掩码=0x${configBits.toString(16)}。"
            }
        }
        if (text.contains("config: no readable config file")) {
            findings += "桌面进程没有读到任何配置：检查 Root 写入结果、包名/用户目录和 SELinux。" +
                "报告中的 config candidates 段会列出每个路径的 stat/access 结果。"
        }
        if (text.contains("access denied")) {
            findings += "至少一个配置路径存在但当前进程无读权限；优先使用 Root 写入桌面私有 files/ 目录。"
        }
        if (text.contains("Dart probe") && text.contains("pool-like ref")) {
            findings += "已采集到 libapp.so 字符串锚点和候选池引用，可用于为 Android 17 构建填写 hook 偏移。"
        } else if (text.contains("Dart probe") && text.contains("not found")) {
            findings += "没有找到预期的界面字符串锚点：该构建可能更改了文案或 Dart 字符串布局，" +
                "需要以实际 libapp.so 重新分析。"
        }
        if (text.contains("waiting for libflutter.so")) {
            findings += "根因：旧版本在等 libflutter.so 装载后才安装 Dart hook，但该启动器 APK 根本没有这个库" +
                "（引擎在 libapp_launcher.so 里，Dart 快照是 libapp.so），所以所有 Dart hook 一个都没装。" +
                "请安装改为只等 libapp.so 的新版本后重新收集。"
        }
        if (text.contains("is not mapped yet")) {
            findings += "采集时 libapp.so 尚未映射：等桌面完全启动后再收集；" +
                "若已启动仍只有这一行而没有 image:/hooked 行，看随后的 watchdog 结论行。"
        }
        if (hasLogSections && text.contains("mapping: libapp.so absent")) {
            findings += "旧脚本的 “mapping: libapp.so absent” 并不可信：APK 内未压缩的 .so 在 maps 里" +
                "显示为 MiuiHome.apk + 文件偏移，不会出现 libapp.so 字样。以 native 日志里的 image:/hooked 行为准。"
        }
        if (text.contains("watchdog: libapp.so still not mapped")) {
            findings += "20 秒内都没找到 libapp.so：该进程可能确实不是 Dart 进程（例如 com.miui.home:widget），" +
                "或注入发生在快照卸载之后。"
        }
        val hookedTargets = Regex(
            "hooked (drawer-tabbar-view|drawer-all-tab|edit-button|" +
                "settings-gear|search-box|swipe-up-arrow)",
        )
            .findAll(text)
            .map { it.groupValues[1] }
            .distinct()
            .toList()
        if (hookedTargets.isNotEmpty()) {
            findings += "已安装 ${hookedTargets.size}/6 类 Dart hook：${hookedTargets.joinToString()}。"
        } else if (text.contains("Dart hooks: target config")) {
            findings += "Dart hook 目标已配置，但日志里没有任何 “hooked <name>” 行：看同一段里每个目标的失败行" +
                "（patch-window reject / Dobby error / pool lookup miss）。"
        }
        if (text.contains("drawer-tabbar-view: shifted children")) {
            findings += "标签栏分类列表已移除“全部”，且 TabBarView 的子页面列表已同步左移；" +
                "标签数量与页面数量一致，首屏是第一个自定义分类。"
        } else if (text.contains("drawer-all-tab: list now len=")) {
            findings += "标签栏共享分类列表已在原地移除“全部”首项；若页面仍多一页，查看是否有 " +
                "drawer-tabbar-view 的 hook 安装失败行或 first page 仍含“全部”。"
        } else if (text.contains("drawer-all-tab: no matching list could be pruned") ||
            text.contains("drawer-all-tab-scan: heap scan finished, 0 match(es)")
        ) {
            findings += "页面/标签栏列表读到了内置“全部”特征，但堆扫描没有找到可修剪的目标：" +
                "通常是 kAllCategoryField10 与当前 libapp.so 不匹配。"
        }
        if (text.contains("drawer-all-tab-scan: heap scan cannot read /proc/self/maps")) {
            findings += "抽屉分类堆扫描无法读取 /proc/self/maps；“全部”不会被移除，但其余开关不受影响。"
        }
        if (text.contains("===== target apk libs =====") &&
            text.contains("lib/arm64-v8a/libapp.so") &&
            !text.contains("lib/arm64-v8a/libflutter.so")
        ) {
            findings += "启动器 APK 没有 libflutter.so（引擎已改名或内建到 libapp_launcher.so）；" +
                "任何以 libflutter.so 为前置条件的旧版本都不会安装 hook。"
        }
        if (text.contains("===== target apk libs =====") &&
            text.contains("no libapp/libflutter/arm64 entries") &&
            !text.contains("lib/arm64-v8a/libapp.so") &&
            !text.contains("lib/arm64-v8a/libflutter.so")
        ) {
            findings += "目标 APK 中没有 libapp.so/libflutter.so：该启动器不是 Flutter 构建，" +
                "Dart hook 方案不适用，需要改用 Java/native hook。"
        }

        if (text.contains("===== module apk =====") &&
            text.contains("entry=META-INF/xposed/java_init.list") &&
            !text.contains("io.github.afhniuk.miuihomeedit.MiuiHomeModule")
        ) {
            findings += "模块 APK 的 META-INF/xposed/java_init.list 中没有入口类；" +
                "请检查 APK 打包，而不是重复设置作用域。"
        }
        if (text.contains("===== module apk =====") &&
            text.contains("entry=META-INF/xposed/module.prop") &&
            (!text.contains("minApiVersion=101") || !text.contains("targetApiVersion=102"))
        ) {
            findings += "模块 APK 的 module.prop 缺少 minApiVersion=101/targetApiVersion=102；" +
                "现代框架会拒绝加载。"
        }
        if (text.contains("===== module apk =====") &&
            text.contains("entry=META-INF/xposed/native_init.list") &&
            !text.contains("lib/arm64-v8a/libmiuihome_edit.so")
        ) {
            findings += "模块 APK 中没有 lib/arm64-v8a/libmiuihome_edit.so；" +
                "native_init 不可能执行。"
        }

        val dbSection = text.substringAfter("===== lsposed database =====", "")
            .substringBefore("===== framework logs =====")
        if (dbSection.isNotBlank() &&
            !dbSection.contains("(sqlite3 unavailable)") &&
            !dbSection.contains("no such table") &&
            !dbSection.contains("Error")
        ) {
            if (!dbSection.contains("io.github.afhniuk.miuihomeedit")) {
                findings += "LSPosed/Vector 的 modules_config.db 中没有本模块：" +
                    "框架管理器尚未启用/注册该 APK，此时不会向桌面注入。"
            }
        }
        if (hasLogSections && text.contains("(no native log found)")) {
            if (nativeLogOpen) {
                findings += "root 脚本没读到 native 日志，但桌面进程正持有 miuihome_edit.log（fd 行可见），" +
                    "说明该目录被 SELinux 拒绝给 su；新版本会把同一份日志镜像到外部目录，" +
                    "面板可直接读取（见 native log (direct read) 段）。"
            } else {
                findings += "没有找到 native 日志：native_init 可能从未在桌面进程执行；" +
                    "先确认模块已在框架中启用且作用域为 $TARGET_PACKAGE。"
            }
        }

        if (hasLogSections &&
            text.contains("(not created: the launcher has not mirrored its log yet)") &&
            !nativeLogOpen
        ) {
            findings += "外部镜像日志不存在，桌面进程也没有打开 miuihome_edit.log 的 fd：" +
                "native_init 没有在桌面进程执行（模块未启用、作用域不含 $TARGET_PACKAGE），" +
                "或者安装的版本还不包含镜像功能。"
        }

        if (text.contains("Dart hooks: arm64 only")) {
            findings += "当前 ABI 不是 arm64-v8a，Dart hook 被跳过。"
        }
        if (text.contains("Dart hooks: invalid libapp.so") ||
            text.contains("libapp.so: no executable PT_LOAD")
        ) {
            findings += "libapp.so 没有可用的可执行段，Dart hook 无法安装。"
        }
        if (text.contains("DobbyHook") && text.contains("failed")) {
            findings += "DobbyHook 安装失败；报告中的 DobbyHook 行包含地址、错误码和原始函数指针。"
        }
        val rootExit = Regex("root\\.exitCode=(-?\\d+)")
            .find(text)
            ?.groupValues
            ?.getOrNull(1)
            ?.toIntOrNull()
        if (rootExit == null || rootExit != 0) {
            if (!collectionComplete) {
                findings += "Root 脚本未正常结束（退出码 $rootExit）；详见上方 root 输出，" +
                    "不要据此判断模块未加载。"
            } else {
                findings += "没有取得 root 诊断数据（su 退出码 $rootExit）；" +
                    "请确认 Magisk/KernelSU 授权后再收集一次。"
            }
        }
        if (findings.isEmpty()) {
            findings += "日志中没有明确结论；请把完整诊断报告发送给开发者。"
        }
        return findings
    }



    /**
     * The script is POSIX sh. Paths are passed as shell-quoted variables rather
     * than interpolated into double-quoted strings, which keeps spaces and
     * apostrophes safe. Every target write uses a temporary file + mv so the
     * launcher never reads a half-written JSON document.
     */
    private fun buildRootScript(
        localFile: File,
        privateDir: String,
        deviceDir: String,
        externalDir: String,
        publicDir: File,
        uid: Int,
    ): String = buildString {
        append("P_DIR=").append(shellQuote(privateDir)).append('\n')
        append("D_DIR=").append(shellQuote(deviceDir)).append('\n')
        append("E_DIR=").append(shellQuote(externalDir)).append('\n')
        append("PUB_DIR=").append(shellQuote(publicDir.absolutePath)).append('\n')
        append("SRC=").append(shellQuote(localFile.absolutePath)).append('\n')
        append("ok=0\n")

        // Device-encrypted storage is available before the user unlocks, which
        // is exactly when the launcher process starts at boot. Write this copy
        // first so a cold start sees the switches before CE and /sdcard mount.
        append("if mkdir -p \"\$D_DIR\" 2>/dev/null && cp \"\$SRC\" \"\$D_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null; then\n")
        append("  chown $uid:$uid \"\$D_DIR\" 2>/dev/null || true\n")
        append("  chown $uid:$uid \"\$D_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  chmod 0644 \"\$D_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  if mv -f \"\$D_DIR/.$CONFIG_FILE.tmp\" \"\$D_DIR/$CONFIG_FILE\" 2>/dev/null; then\n")
        append("    command -v restorecon >/dev/null 2>&1 && restorecon \"\$D_DIR/$CONFIG_FILE\" >/dev/null 2>&1 || true\n")
        append("    echo DEVICE_OK\n")
        append("    ok=1\n")
        append("  fi\n")
        append("fi\n")

        append("if mkdir -p \"\$P_DIR\" 2>/dev/null && cp \"\$SRC\" \"\$P_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null; then\n")
        // If files/ did not exist, mkdir created it as root; hand it back so the
        // launcher can still write its own files there.
        append("  chown $uid:$uid \"\$P_DIR\" 2>/dev/null || true\n")
        append("  chown $uid:$uid \"\$P_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  chmod 0644 \"\$P_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  if mv -f \"\$P_DIR/.$CONFIG_FILE.tmp\" \"\$P_DIR/$CONFIG_FILE\" 2>/dev/null; then\n")
        append("    command -v restorecon >/dev/null 2>&1 && restorecon \"\$P_DIR/$CONFIG_FILE\" >/dev/null 2>&1 || true\n")
        append("    echo PRIVATE_OK\n")
        append("    ok=1\n")
        append("  fi\n")
        append("fi\n")

        append("if mkdir -p \"\$E_DIR\" 2>/dev/null && cp \"\$SRC\" \"\$E_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null; then\n")
        append("  chown $uid:$uid \"\$E_DIR\" 2>/dev/null || true\n")
        append("  chmod 0644 \"\$E_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  chown $uid:$uid \"\$E_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  if mv -f \"\$E_DIR/.$CONFIG_FILE.tmp\" \"\$E_DIR/$CONFIG_FILE\" 2>/dev/null; then\n")
        append("    echo EXTERNAL_OK\n")
        append("    ok=1\n")
        append("  fi\n")
        append("fi\n")

        append("if mkdir -p \"\$PUB_DIR\" 2>/dev/null && cp \"\$SRC\" \"\$PUB_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null; then\n")
        append("  chmod 0644 \"\$PUB_DIR/.$CONFIG_FILE.tmp\" 2>/dev/null || true\n")
        append("  if mv -f \"\$PUB_DIR/.$CONFIG_FILE.tmp\" \"\$PUB_DIR/$CONFIG_FILE\" 2>/dev/null; then\n")
        append("    echo PUBLIC_OK\n")
        append("  fi\n")
        append("fi\n")

        append("if [ \"\$ok\" = 1 ]; then echo ROOT_DONE; else echo ROOT_FAILED; fi\n")
    }

    private fun buildDetail(root: ShellResult): String = when {
        !root.available -> "su 不可用：" + root.output.take(200)
        root.timedOut -> "su 超时：请确认 Root 授权弹窗后重试"
        !root.output.contains("PRIVATE_OK") && !root.output.contains("DEVICE_OK") &&
            !root.output.contains("EXTERNAL_OK") ->
            "Root 输出：" + root.output.take(500)
        else -> ""
    }

    private fun writeAtomic(file: File, contents: String): Boolean {
        return try {
            val parent = file.parentFile ?: return false
            if (!parent.exists() && !parent.mkdirs()) {
                return false
            }
            val tmp = File(parent, ".${file.name}.tmp")
            FileOutputStream(tmp).use { stream ->
                stream.write(contents.toByteArray(Charsets.UTF_8))
                stream.fd.sync()
            }
            if (tmp.renameTo(file)) {
                true
            } else {
                if (file.exists()) {
                    file.delete()
                }
                val renamed = tmp.renameTo(file)
                if (!renamed) {
                    tmp.delete()
                }
                renamed
            }
        } catch (e: Exception) {
            Log.w(TAG, "Could not write ${file.absolutePath}", e)
            false
        }
    }

    private fun runSu(
        script: String,
        maxOutputChars: Int = 256 * 1024,
        timeoutSeconds: Long = ROOT_TIMEOUT_SECONDS,
    ): ShellResult {
        return try {
            val process = ProcessBuilder("su", "-c", script)
                .redirectErrorStream(true)
                .start()
            try {
                process.outputStream.close()
            } catch (ignored: Exception) {
                // su does not read stdin; failure to close it is harmless.
            }
            val output = StringBuilder()
            val reader = Thread {
                try {
                    process.inputStream.bufferedReader().use { input ->
                        val buffer = CharArray(1024)
                        var remaining = maxOutputChars
                        var truncated = false
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) {
                                break
                            }
                            if (remaining <= 0) {
                                if (!truncated) {
                                    synchronized(output) {
                                        output.append("\n... output truncated at ")
                                            .append(maxOutputChars).append(" chars ...\n")
                                    }
                                    truncated = true
                                }
                                continue
                            }
                            val keep = minOf(count, remaining)
                            synchronized(output) {
                                output.append(String(buffer, 0, keep))
                            }
                            remaining -= keep
                        }
                    }
                } catch (ignored: Exception) {
                    // The process may have been killed after a timeout.
                }
            }
            reader.isDaemon = true
            reader.start()

            val finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS)
            if (!finished) {
                process.destroyForcibly()
            }
            try {
                reader.join(3000)
            } catch (ignored: InterruptedException) {
                // The result below is still usable.
            }
            val text = synchronized(output) { output.toString() }
            if (finished) {
                ShellResult(process.exitValue(), text, false)
            } else {
                ShellResult(-1, text, true)
            }
        } catch (e: Exception) {
            ShellResult(ROOT_UNAVAILABLE, e.message ?: "su unavailable", false)
        }
    }

    private fun shellQuote(value: String): String =
        "'" + value.replace("'", "'\\''") + "'"
}
