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
 * Six independent switches. The panel talks to the native module only through
 * the JSON file; it never tries to bind to the launcher process.
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        config = ConfigStore.loadLocal(this)
        setContentView(buildUi())
        refreshTargetState()
        statusView.text = "开关默认全部关闭。打开一个开关会立即保存，并在约 2 秒内被 com.miui.home 读取。" +
            "若桌面已重启仍无变化，请点“收集诊断日志”；日志若提示 no target offsets，说明当前构建的 hook 目标尚未配置；若提示模块未加载，请确认使用支持 API 101+ 的 Vector（LSPosed 1.9.x 不支持本模块）。"
    }

    override fun onResume() {
        super.onResume()
        if (::statusView.isInitialized) {
            refreshTargetState()
        }
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
            text = "五个开关互相独立，默认关闭。每次切换都会原子写入 JSON。"
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
            title = "隐藏抽屉“全部”标签",
            description = "从抽屉分类列表移除“全部”，保留自定义标签；打开抽屉直接显示第一个自定义标签。",
            checked = config.hideDrawerAllTab,
        ) { checked ->
            config = config.copy(hideDrawerAllTab = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏桌面编辑按钮",
            description = "长按桌面进入编辑模式时，隐藏右上角/底部的编辑入口。",
            checked = config.hideEditButton,
        ) { checked ->
            config = config.copy(hideEditButton = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏桌面设置齿轮",
            description = "隐藏桌面设置入口（齿轮图标）。",
            checked = config.hideSettingsGear,
        ) { checked ->
            config = config.copy(hideSettingsGear = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏搜索框",
            description = "桌面与抽屉的搜索框返回空 Widget。",
            checked = config.hideSearchBox,
        ) { checked ->
            config = config.copy(hideSearchBox = checked)
            persist()
        }
        addSwitch(
            root,
            title = "隐藏上滑箭头",
            description = "应用抽屉的上滑提示箭头返回 false。",
            checked = config.hideSwipeUpArrow,
        ) { checked ->
            config = config.copy(hideSwipeUpArrow = checked)
            persist()
        }
        root.addView(TextView(this).apply {
            text = "写入位置（桌面会读取最新的有效文件）：\n" +
                "1. /data/user/<user>/com.miui.home/files/miuihome_edit.json\n" +
                "2. /storage/emulated/<user>/Android/data/com.miui.home/files/miuihome_edit.json\n" +
                "3. /storage/emulated/<user>/MiuiHomeEdit/miuihome_edit.json（无 Root 备用）"
            textSize = 11f
            setTextColor(Color.rgb(110, 116, 128))
            setTypeface(Typeface.MONOSPACE)
            setPadding(0, dp(14), 0, 0)
        }, matchWidth())

        allFilesButton = Button(this).apply {
            text = "授予“所有文件访问”（无 Root 时的备用）"
            setOnClickListener { ConfigStore.requestAllFilesAccess(this@MainActivity) }
        }
        root.addView(allFilesButton, matchWidth(top = 14))

        restartButton = Button(this).apply {
            text = "重启 com.miui.home"
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
            text = "导出诊断报告…"
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
        statusView.text = "正在保存并写入 com.miui.home 可读取的路径…"
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
        val installed = ConfigStore.targetInstalled(this)
        val root = when {
            report == null -> "保存配置时检测"
            report.rootAvailable -> "可用"
            else -> "不可用"
        }
        val allFiles = if (ConfigStore.allFilesAccessGranted()) "已授予" else "未授予"
        targetView.text = "目标应用：com.miui.home ${if (installed) "已安装" else "未找到"}\n" +
            "Android：${Build.VERSION.RELEASE}（SDK ${Build.VERSION.SDK_INT}，" +
            "${Build.SUPPORTED_ABIS.firstOrNull() ?: "?"}）\n" +
            "Root：$root\n" +
            "所有文件访问：$allFiles"
        allFilesButton?.visibility =
            if (ConfigStore.allFilesAccessGranted()) View.GONE else View.VISIBLE
    }

    private companion object {
        const val REQUEST_EXPORT_DIAGNOSTICS = 1001
    }
}
