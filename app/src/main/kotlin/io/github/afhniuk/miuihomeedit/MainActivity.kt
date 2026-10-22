package io.github.afhniuk.miuihomeedit

import android.annotation.SuppressLint
import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import java.io.File

/**
 * The five independent switches. The panel talks to the native module only
 * through the JSON file; it never tries to bind to the launcher process.
 *
 * A change is persisted immediately, but the native side reads the document
 * exactly once when com.miui.home starts. Until that process is restarted, the
 * already-latched selection keeps applying; after the restart the new JSON is
 * latched for the whole next session.
 *
 * The panel is intentionally Chinese-only, so the labels are kept inline rather
 * than duplicated in strings.xml; the lint I18N check is not useful here.
 */
@SuppressLint("SetTextI18n")
class MainActivity : Activity() {
    private lateinit var config: LauncherConfig
    private lateinit var statusView: TextView
    private lateinit var targetView: TextView
    private val switches = mutableListOf<Switch>()
    private var restartButton: Button? = null
    private var allFilesButton: Button? = null
    private var diagnosticsButton: Button? = null
    private var copyDiagnosticsButton: Button? = null
    private var exportDiagnosticsButton: Button? = null
    private var lastDiagnostics: ConfigStore.DiagnosticReport? = null
    private var saving = false

    /** null while the background query is still running. */
    private var rootGranted: Boolean? = null
    private var scopeState: ConfigStore.ScopeState? = null
    private var checkingEnvironment = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        config = ConfigStore.loadLocal(this)
        setContentView(buildUi())
        refreshTargetState()
        statusView.text = "开关默认全部关闭。打开一个开关会立即持久化保存，但不会热更新当前桌面进程；" +
            "下一次重启 com.miui.home 时才会读取新配置，并在该进程结束前一直保持这份启动快照。" +
            "若桌面已重启仍无变化，请点“收集诊断日志”；日志若提示 no target offsets，说明当前构建的 hook 目标尚未配置；若提示模块未加载，请确认使用支持 API 101+ 的 Vector（LSPosed 1.9.x 不支持本模块）。"
    }

    override fun onResume() {
        super.onResume()
        if (::statusView.isInitialized) {
            refreshTargetState()
            checkEnvironment()
        }
    }

    /**
     * Root and the framework scope can change while the panel is in the
     * background, so both are queried again on every entry. su may block on the
     * manager's prompt, therefore the query runs off the main thread.
     */
    private fun checkEnvironment() {
        if (checkingEnvironment) {
            return
        }
        checkingEnvironment = true
        rootGranted = null
        scopeState = null
        refreshTargetState()
        Thread {
            val root = ConfigStore.rootGranted()
            // Without root neither the framework database nor the launcher's
            // memory map can be read, so no scope can be active.
            val scope = if (root) {
                ConfigStore.scopeState(this@MainActivity)
            } else {
                ConfigStore.ScopeState.Disabled
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                checkingEnvironment = false
                rootGranted = root
                scopeState = scope
                refreshTargetState()
            }
        }.start()
    }

    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(28))
            setBackgroundColor(Color.rgb(248, 249, 252))
        }

        root.addView(TextView(this).apply {
            text = "桌面精简控制面板"
            textSize = 24f
            setTextColor(Color.rgb(32, 36, 44))
            setTypeface(typeface, Typeface.BOLD)
        })
        root.addView(TextView(this).apply {
            text = "使用方法：授予该应用root权限并在LSPosed内启用该模块。回到本应用后勾选需要启用的功能并点击重启桌面按钮。"
            textSize = 14f
            setTextColor(Color.rgb(96, 102, 112))
            setPadding(0, dp(6), 0, dp(12))
        })

        targetView = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(70, 76, 88))
            setBackgroundColor(Color.WHITE)
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        root.addView(targetView, matchWidth(top = 4))

        addSwitch(
            root,
            title = "隐藏“全部”标签页",
            description = "从抽屉模式的标签栏中移除“全部”标签，并将进入抽屉模式的首页面改为首个自定义标签页。",
            checked = config.hideDrawerAllTab,
        ) { checked ->
            config = config.copy(hideDrawerAllTab = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏编辑按钮",
            description = "隐藏排列在每个标签页最后一位的编辑按钮。",
            checked = config.hideEditButton,
        ) { checked ->
            config = config.copy(hideEditButton = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏设置齿轮",
            description = "隐藏位于抽屉模式标签栏最右侧的设置齿轮。",
            checked = config.hideSettingsGear,
        ) { checked ->
            config = config.copy(hideSettingsGear = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏搜索框",
            description = "隐藏抽屉模式位于每个标签页下方的应用搜索框。",
            checked = config.hideSearchBox,
        ) { checked ->
            config = config.copy(hideSearchBox = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏上滑箭头",
            description = "隐藏手机主页下方用于引导进入抽屉模式的上滑箭头。",
            checked = config.hideSwipeUpArrow,
        ) { checked ->
            config = config.copy(hideSwipeUpArrow = checked)
            persist()
        }
        allFilesButton = Button(this).apply {
            text = "授予“所有文件访问权限”（无 Root 时的备用）"
            setOnClickListener { ConfigStore.requestAllFilesAccess(this@MainActivity) }
        }
        root.addView(allFilesButton, matchWidth(top = 14))

        restartButton = Button(this).apply {
            text = "重启桌面"
            setOnClickListener { restartLauncher() }
        }
        root.addView(restartButton, matchWidth(top = 8))

        diagnosticsButton = Button(this).apply {
            text = "收集诊断日志（需要 Root）"
            setOnClickListener { collectDiagnostics() }
        }
        root.addView(diagnosticsButton, matchWidth(top = 16))

        copyDiagnosticsButton = Button(this).apply {
            text = "复制诊断报告"
            isEnabled = false
            setOnClickListener { copyDiagnostics() }
        }
        root.addView(copyDiagnosticsButton, matchWidth(top = 8))

        exportDiagnosticsButton = Button(this).apply {
            text = "导出诊断报告"
            isEnabled = false
            setOnClickListener { exportDiagnostics() }
        }
        root.addView(exportDiagnosticsButton, matchWidth(top = 8))

        statusView = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.rgb(60, 66, 78))
            setBackgroundColor(Color.WHITE)
            setPadding(dp(12), dp(10), dp(12), dp(10))
            setTextIsSelectable(true)
        }
        root.addView(statusView, matchWidth(top = 16))

        return ScrollView(this).apply {
            isFillViewport = true
            addView(
                root,
                ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                ),
            )
        }
    }

    private fun addSwitch(
        parent: LinearLayout,
        title: String,
        description: String,
        checked: Boolean,
        onChanged: (Boolean) -> Unit,
    ) {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(8), dp(12), dp(8))
            setBackgroundColor(Color.WHITE)
        }
        val toggle = Switch(this).apply {
            text = title
            textSize = 16f
            setTextColor(Color.rgb(32, 36, 44))
            isChecked = checked
        }
        toggle.setOnCheckedChangeListener { _, value -> onChanged(value) }
        switches += toggle
        box.addView(toggle, matchWidth())
        box.addView(TextView(this).apply {
            text = description
            textSize = 12f
            setTextColor(Color.rgb(104, 110, 122))
            setPadding(dp(4), dp(2), 0, 0)
        }, matchWidth())
        parent.addView(box, matchWidth(top = 8))
    }

    private fun matchWidth(top: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply {
            topMargin = dp(top)
        }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun persist() {
        if (saving) {
            return
        }
        saving = true
        setControlsEnabled(false)
        statusView.text = "正在保存并写入 com.miui.home 可读取的路径…\n保存后需重启桌面才会应用；当前桌面进程会继续使用启动时的配置快照。"
        val snapshot = config
        Thread {
            val report = ConfigStore.save(this@MainActivity, snapshot)
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                saving = false
                setControlsEnabled(true)
                statusView.text = report.describe()
                refreshTargetState(report)
            }
        }.start()
    }

    private fun setControlsEnabled(enabled: Boolean) {
        switches.forEach { it.isEnabled = enabled }
        restartButton?.isEnabled = enabled
        diagnosticsButton?.isEnabled = enabled
        val hasReport = lastDiagnostics != null
        copyDiagnosticsButton?.isEnabled = enabled && hasReport
        exportDiagnosticsButton?.isEnabled = enabled && hasReport
    }

    private fun restartLauncher() {
        statusView.text = "正在停止 com.miui.home…"
        Thread {
            val result = ConfigStore.restartLauncher(this@MainActivity)
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                statusView.text = if (result.exitCode == 0) {
                    "已停止 com.miui.home。按 Home 键或重新解锁后，系统会重新启动桌面并读取新配置。"
                } else {
                    "重启失败（退出码 ${result.exitCode}）：${result.output.take(300)}"
                }
            }
        }.start()
    }

    private fun collectDiagnostics() {
        if (saving) {
            return
        }
        saving = true
        setControlsEnabled(false)
        statusView.text = "正在收集诊断日志…\n需要读取桌面私有日志和 logcat；请确认 Root 授权弹窗。"
        val started = System.currentTimeMillis()
        Thread {
            val result = runCatching { ConfigStore.collectDiagnostics(this@MainActivity) }
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                saving = false
                val report = result.getOrNull()
                if (report == null) {
                    val error = result.exceptionOrNull()
                    statusView.text = "收集诊断日志失败：${error?.javaClass?.simpleName}: " +
                        "${error?.message ?: "unknown"}"
                } else {
                    lastDiagnostics = report
                    statusView.text = report.describe() +
                        "\n耗时 ${(System.currentTimeMillis() - started) / 1000} 秒。"
                }
                setControlsEnabled(true)
                refreshTargetState()
            }
        }.start()
    }

    private fun copyDiagnostics() {
        val report = lastDiagnostics ?: return
        val clipboard = getSystemService(ClipboardManager::class.java) ?: return
        clipboard.setPrimaryClip(
            ClipData.newPlainText("MiuiHomeEdit diagnostics", report.text),
        )
        statusView.text = "诊断报告已复制到剪贴板（${report.text.length} 字符）。"
    }

    private fun exportDiagnostics() {
        val report = lastDiagnostics ?: return
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "text/plain"
            putExtra(Intent.EXTRA_TITLE, File(report.path).name)
        }
        try {
            startActivityForResult(intent, REQUEST_EXPORT_DIAGNOSTICS)
        } catch (e: Exception) {
            statusView.text = "无法打开导出界面：${e.javaClass.simpleName}: ${e.message}"
        }
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_EXPORT_DIAGNOSTICS || resultCode != RESULT_OK) {
            return
        }
        val report = lastDiagnostics ?: return
        val uri = data?.data ?: return
        try {
            contentResolver.openOutputStream(uri)?.use { stream ->
                stream.write(report.text.toByteArray(Charsets.UTF_8))
            } ?: throw IllegalStateException("openOutputStream returned null")
            statusView.text = "诊断报告已导出到：$uri"
        } catch (e: Exception) {
            statusView.text = "导出失败：${e.javaClass.simpleName}: ${e.message}"
        }
    }


    private fun refreshTargetState(report: ConfigStore.SaveReport? = null) {
        // A finished save is itself a root query: reuse its answer instead of
        // starting another su round-trip.
        if (report != null) {
            rootGranted = report.rootAvailable
        }
        val root = when (rootGranted) {
            true -> "已授予"
            false -> "未授予"
            null -> "检测中…"
        }
        val allFiles = if (ConfigStore.allFilesAccessGranted()) "已授予" else "未授予"
        val scope = when (scopeState) {
            ConfigStore.ScopeState.Enabled -> "已启用"
            ConfigStore.ScopeState.Disabled -> "未启用"
            null -> "检测中…"
        }
        targetView.text = "Android：${Build.VERSION.RELEASE}（SDK ${Build.VERSION.SDK_INT}，" +
            "${Build.SUPPORTED_ABIS.firstOrNull() ?: "?"}）\n" +
            "Root：$root\n" +
            "所有文件访问权限：$allFiles\n" +
            "LSPosed作用域：$scope"
        allFilesButton?.visibility =
            if (ConfigStore.allFilesAccessGranted()) View.GONE else View.VISIBLE
    }

    private companion object {
        const val REQUEST_EXPORT_DIAGNOSTICS = 1001
    }
}
