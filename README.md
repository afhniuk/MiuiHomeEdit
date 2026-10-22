# MiuiHomeEdit

面向 HyperOS 4 桌面启动器 `com.miui.home`（Android 17 / SDK 37；目标构建 RELEASE-8.01.02.7719，请以设备实际版本为准）的 libxposed（API 101+）模块。应用包名为 `io.github.afhniuk.miuihomeedit`。

启动器本身是 Flutter 应用，实际界面逻辑位于 AOT 编译的 Dart 载荷 `libapp.so` 中，几乎没有可用的 Java 反射入口。本模块的界面 hook 全部在启动器进程内的 C++ 中完成。Kotlin 部分负责控制面板、写入配置和收集诊断日志。

## 当前状态（重要）

`dart_hooks.cpp` 的 `namespace target` 已按目标构建（RELEASE-8.01.02.7719 的 `libapp.so`）填写完毕，五个开关都会真正安装 inline hook：

| # | 功能 | `libapp.so` 内偏移 | 目标函数 |
| --- | --- | --- | --- |
| 1 | 抽屉“全部”分类 | `0x14be89c` + `0x12d12a0` | `_AllAppsCategoryTabBar.build` 修剪分类列表；`_TabBarViewState.build` 同步左移缓存的页面 children |
| 2 | 编辑按钮 | `0x14c2588` | `NormalAppListWidget._buildEditWidget`（仅用于隐藏图块；分类对齐由第 1 项在抽屉数据层完成） |
| 3 | 设置齿轮 | `0x14bec50` | `_AllAppsSettingsIcon.build` |
| 4 | 搜索框 | `0x149df2c` | `SearchBarContainerWidget.build`（桌面与抽屉共用） |
| 5 | 上滑箭头 | `0x1176bac` + `0x117735c` | `LauncherIndicatorState._showAllAppsArrowIndicator`，并额外挂 `_buildAllAppsIndicator` 作为 Widget 兜底 |

这些偏移只对上面这一个构建成立：Dobby 会改写目标函数入口的前 16 字节，偏差几个字节就会变成启动循环。换版本后必须用 build-id 重新核对。每个目标成功安装时打印一行 `hooked <name> at 0x... (original 0x...)`；某个目标失败只跳过它自己，其余照常安装。

除五个功能开关外，`dart_hooks.cpp` 还挂了一个只读诊断 hook `editor-init`（`AppCategorySelectController.onInit` @ `0xefbda0`）。它只做一件事：把 receiver `+0x20` 处的分类 id 和 class id 打进日志。这个 id 由“编辑”图块所在页面下发（未装箱的 64 位整数），编辑器按它解析分类；它与该页分类 id 一致，就说明图块交接正确。hook 不写任何对象，也不改变桌面行为。

第 1 项的最终做法使用两个配合的 hook。`_AllAppsCategoryTabBar.build`（`0x14be89c`）的 pre-hook 从 receiver（x1）的 `+0x0c` 取出真实分类名称列表（本构建是 `List<String>`，首元素为内置“全部”的名称对象），校验首元素 `+0x10` 是否为“全部”，再按 class id + 长度 + 首元素特征扫描 Dart 堆，把所有匹配列表原地 `memmove` 掉首项并改写长度 Smi。这样标签栏、TabController 和标签数据变成 N-1；随后同一 hook 还会把页面用的 id 镜像与分类对象列表一起修剪（见下文“编辑按钮”）。

只改标签列表仍会留下一个旧问题：`AllAppsListWidget` 已经把 N 个页面 child 交给 `_TabBarViewState` 并在 state 中缓存，标签虽然变成 N-1，页面 0 仍是“全部”。因此 `_TabBarViewState.build`（`0x12d12a0`）的 pre-hook 读取 state 原始对象 `+0x1c` 处的 children `List`，在长度大于已修剪分类长度时把元素整体左移一位并写回长度 N-1；`g_pruned_category_length` 记录当前 N-1，后续同一列表或已经是 N-1 的新列表不会被重复左移。实测首屏直接显示“社交通讯”等第一个自定义分类，最后一个标签也能正常打开。

这个左移现在**只依赖列表自身的长度**来保证幂等：长度已经 ≤N-1 的列表直接跳过。早期实现还缓存了最近 32 个“已左移”的列表指针；长时间使用后 Dart GC 会回收旧页面列表，并把同一地址分配给新列表，新列表会被误判为已处理而保留 N 项，页面 0 就再次变成“全部”。该指针缓存已删除，这正是“使用一段时间后页面重新出现”的根因之一。

第 2 项（编辑按钮）的分类错位已修复，根因不在“编辑”图块本身。页面 children 左移后，点第 k 个标签页的“编辑”曾会打开“上一个”分类（第 1 页打开“全部”，第 4 页打开“游戏”）。原因是抽屉同时维护两份平行数据：标签栏消费的 `List<String>` 名称列表，以及页面组件按页取分类 id 的镜像列表（`List<int>` 与分类对象列表各一份）。此前只修剪了名称列表，页面仍从“含全部”的镜像里取 id，第 i 页因此拿到 `ids[i]`（上一页的分类 id），编辑器再按这个 id 解析，就落在上一个分类上。

修复放在同一次 `_AllAppsCategoryTabBar.build` pre-hook 里，紧接名称列表的修剪之后：

* **id 镜像**：growable list（class 92）、长度与名称列表相同、元素全是小偶数 Smi（分类 id 的 Smi 编码）、互不相同且首项为 0，即形如 `0,1,2,1001,4,3,5,6,8` 的 `List<int>`；删掉首项（`memmove` + 长度 Smi -1）。
* **分类对象列表**：长度相同，且首元素是“引用内置『全部』字符串”的对象（`+0x08..+0x18` 中某个引用字段指向 `+0x10` 载荷为 `kAllCategoryField10` 的字符串）；同样删掉首项。`List<String>` 名称列表不会误伤：它的首元素就是“全部”字符串本身，载荷内联在对象里而不是指针。

两种判定都要求长度与名称列表一致，且删掉首项后长度立刻变为 N-1，因此重建出来的新列表会再次命中、已修剪的列表不会重复处理。实测（8 个自定义分类）：1 社交通讯、2 信息流、3 游戏、4 实用工具（id 1001）、5 学习（id 3）、6 少用工具、7 root类、8 lsp类，逐页点“编辑”都打开本页分类。

分类 id 并不连续（1001 夹在 2 和 4 之间），所以不能在编辑器入口把 id 加一；必须让镜像本身少一项。`editor-init` 诊断因此保留为验证手段：入口处 receiver `+0x20` 是未装箱的 64 位分类 id（`onInit` 用 `sbfiz x0, x6, #1, #31` 装箱），日志打印该 id，与页面分类 id 一致即说明“编辑”图块交接正确。

`_buildEditWidget`（`0x14c2588`）只保留“隐藏编辑按钮”的用途：它的 receiver（class 8001）确实不持有分类列表，图块要编辑的分类是抽屉按页下发、由编辑器自己按 id 解析的。打开 `hide_edit_button` 后图块从界面消失，这一点已实测确认。

## 运行期自愈（覆盖整个桌面进程生命周期）

inline hook 只在启动时补一次代码并不够：桌面进程可以一直活着，但补丁仍会在下面两种情况下悄悄失效。

* `libapp.so` 位于 `MiuiHome.apk` 内，其可执行段是文件映射。内存压力下页面被回收（或映射被重建）时，缺页可能从 APK 重新读入原始字节，Dobby 写在函数入口前 16 字节的跳转被覆盖；所有 hook 同时失效，而进程和已缓存的 Dart 对象都仍然有效。
* 桌面若在同一进程内重建 Flutter 引擎/isolate，`libapp.so` 会以新的 load bias 重新映射，旧补丁留在旧映像上；旧 isolate 的 `_SizedBox`、`NullObject` 等指针也全部作废。

`native_init` 启动的后台线程不再在首次安装后退出，而是每秒调用一次 `miuihome::dart::EnsureHealthy()`：

* `libapp.so` 的 base 变化时，清空 isolate 相关缓存（空 Widget、`false`、已修剪长度），并在新映像上重新安装全部 hook；
* base 不变时逐项比较目标入口的 16 字节：仍是 Dobby 补丁则跳过；恢复成安装前的原始 prologue 则把保存的补丁写回（trampoline 仍然有效，不需要再次 `DobbyHook`）；两者都不是则只告警，不猜测、不写入；
* 框架的 `dlopen` 回调看到 `libapp.so` 时也会同步检查一次，覆盖“第二个引擎在回调返回后立刻执行 Dart”的情况。

空 Widget 也不再永久缓存：`GetEmptyWidget()` 每次都用 class id `7713`（该构建的 `_SizedBox`）校验缓存对象，失效就从当前 PP 的对象池重新解析。`false` 改为每次从 x22 派生（`NullObject+0x30`），不再缓存跨 isolate 的指针。健康检查正常时保持安静，只在修复或异常时输出 `health: ...`。

## 功能

控制面板提供五个互相独立的开关，默认全部关闭：

| 开关 | JSON 键 | 目标与效果 |
| --- | --- | --- |
| 隐藏抽屉“全部”标签 | `hide_drawer_all_tab` | 从标签栏共享的分类列表移除内置“全部”项；只保留自定义标签，第一个页面即第一个自定义标签 |
| 隐藏桌面编辑按钮 | `hide_edit_button` | 编辑模式入口的 Widget 构建函数返回空 Widget |
| 隐藏桌面设置齿轮 | `hide_settings_gear` | 设置入口的 Widget 构建函数返回空 Widget |
| 隐藏搜索框 | `hide_search_box` | `SearchBarContainerWidget.build` 返回空 Widget，桌面与抽屉搜索框一起隐藏 |
| 隐藏上滑箭头 | `hide_swipe_up_arrow` | 上滑提示 predicate 返回静态 `false` 对象 |

配置缺失、文件不可读、JSON 损坏或没有任何有效候选文件时，五个功能都保持关闭。JSON 中出现未知键不会导致文件失效，便于后续版本扩展。

## 兼容性

* 目标为 `com.miui.home`（HyperOS 4，Android 17 / API 37）。`compileSdk`、`targetSdk`、`minSdk` 分别为 37、37、35。
* 启动器是 64 位 Flutter 进程；Dart hook 只在 `arm64-v8a` 编译。APK 仍带 `armeabi-v7a` 变体，但该 ABI 只会记录“arm64 only”日志。
* 16 KB 页：NDK r30 默认支持，CMake 与 Gradle 都显式传入 `-Wl,-z,max-page-size=16384`；Android 17 设备上不要移除。
* 使用现代 libxposed 模块格式，要求 API 101+ 且识别 `META-INF/xposed/module.prop` 的框架（例如 Vector）。LSPosed 1.9.x 读取的是旧式 `assets/xposed_init` / `xposedminversion`，不会列出本 APK；如果模块管理器里看不到本模块，先确认框架是否支持 API 101+，而不是反复勾选作用域。
* `staticScope=true`：管理器只允许把作用域设为 `com.miui.home`。
* `autoHotReload=false`：native inline hook 无法卸载，热重载会让旧代码的补丁继续留在进程里。
* `exceptionMode=protective`：hook 抛出的异常会被上报并吞掉，而不是让桌面崩溃。
* 锁定的启动器版本之外不要随意使用；Dart AOT 偏移和对象池地址与具体构建绑定。Android 17 的 `libapp.so` 与 Android 15/16 构建不保证兼容。
* 抽屉分类修剪依赖内置“全部”元素的 `+0x10` 字段（硬编码 `kAllCategoryField10 = 0x90e85168`），并依赖 `_TabBarViewState` 的 growable-list children 字段在原始对象 `+0x1c`（当前构建 class id 92）。换 `libapp.so` 后必须重新确认这些位置；不匹配时 hook 只写日志并跳过修剪，“全部”标签/页面会保留，但不会崩溃。
* **编辑按钮分类对齐**：页面分类 id 的镜像列表（`List<int>` 与分类对象列表）与名称列表在同一处修剪，判定条件见上文“编辑按钮”一节。若新版 `libapp.so` 改了分类对象里“名称”字段的位置，`drawer-ids: shifted ... category list` 的数量会变成 0，此时标签栏仍正确少一项，但“编辑”会退回到打开上一个分类；日志里 `editor-init: ... category id=N` 与页面分类 id 不一致即可发现。

## 项目结构

| 路径 | 作用 |
| --- | --- |
| `app/src/main/resources/META-INF/xposed/module.prop` | 现代模块描述：`minApiVersion=101`、`targetApiVersion=102`、`staticScope=true`、`autoHotReload=false`、`exceptionMode=protective` |
| `app/src/main/resources/META-INF/xposed/scope.list` | 唯一允许的作用域：`com.miui.home` |
| `app/src/main/resources/META-INF/xposed/java_init.list` | Java 入口类：`io.github.afhniuk.miuihomeedit.MiuiHomeModule` |
| `app/src/main/resources/META-INF/xposed/native_init.list` | `libmiuihome_edit.so`：框架需要调用其 `native_init()` 的库 |
| `app/src/main/java/io/github/afhniuk/miuihomeedit/MiuiHomeModule.java` | 框架入口：加载 native 库、报告模块已加载 |
| `app/src/main/kotlin/io/github/afhniuk/miuihomeedit/MainActivity.kt` | 控制面板：五个开关、保存状态、重启桌面、收集/复制/导出诊断报告 |
| `app/src/main/kotlin/io/github/afhniuk/miuihomeedit/LauncherConfig.kt` | 五个布尔值及与 `module_config.cpp` 共享的 JSON 契约 |
| `app/src/main/kotlin/io/github/afhniuk/miuihomeedit/ConfigStore.kt` | 本地副本、原子写入、root 脚本、公共目录回退、诊断收集和 `am force-stop` |
| `app/src/main/cpp/module_config.h` / `.cpp` | 无第三方依赖的 JSON 读取器、逐路径诊断，以及 hook 查询的功能位 |
| `app/src/main/cpp/main.cpp` | `native_init()`、`dlopen` 回调、镜像发现和环境日志 |
| `app/src/main/cpp/aot_scanner.h` | ELF 段查找、ARM64 掩码扫描、指令解码、对象池与 `bl` 交叉引用 |
| `app/src/main/cpp/dart_hooks.h` / `dart_hooks.cpp` | Dobby inline hook、八个注入点（七个名称）、ABI 安全替换函数和 fail-open 校验 |
| `app/src/main/cpp/diag.h` / `diag.cpp` | logcat + 文件双写、设备/ROM/ABI 信息、ELF 段/build-id、Dart 字符串锚点探测 |
| `app/src/main/cpp/log.h` | `LOGI` / `LOGW` / `LOGE` / `LOGD`，统一转到 `diag::Writef()` |
| `app/src/main/cpp/CMakeLists.txt` | 构建 `libmiuihome_edit.so`（C++17，关闭异常和 RTTI），链接 Dobby 并保留 Dart ABI 寄存器 |
| `third_party/dobby/` | 固定提交的 Dobby（Apache-2.0）；来源见 `UPSTREAM.txt` |
| `app/src/main/keepRules/rules.keep` | R8 保留框架入口类 |

`module.prop`、`java_init.list`、`native_init.list`、`scope.list` 现在只包含字段/值，不含注释或空行：不同框架对这些 list 文件的解析严格程度不同，注释可能被当成类名或库名，导致入口类“找不到”。

这里刻意**不写** `xposedmodule`、`xposedminversion`、`xposeddescription`、`xposedscope` 等旧式 meta-data：现代框架通过 `META-INF/xposed/module.prop` 发现模块，旧式标记会让部分管理器按旧式路径读取并不存在的 `assets/xposed_init`。入口 Activity 额外带 `de.robv.android.xposed.category.MODULE_SETTINGS` 和 `android.intent.category.INFO`，便于管理器打开控制面板。

## 为什么 APK 里仍然需要 dex

不存在完全无 dex 的模块。LSPosed 和 Vector 的加载器都会读取入口类列表（现代模块是 `META-INF/xposed/java_init.list`，旧式模块是 `assets/xposed_init`）；如果列表中的类无法从 `classes*.dex` 加载，APK 会被判定为不可用。没有 dex 的 APK 可以安装，但永远不会出现在模块列表中，也无法启用。

框架入口类仍是 `MiuiHomeModule`，它只做一件事：调用 `System.loadLibrary("miuihome_edit")`。同一个 APK 还包含 Kotlin 控制面板（`MainActivity`、`LauncherConfig`、`ConfigStore`）以及 `kotlin-stdlib`；这些类不属于 hook 路径，启动器进程不会加载面板代码。真正的 hook 全部位于 `app/src/main/cpp/main.cpp`（引擎/导出符号）和 `app/src/main/cpp/dart_hooks.cpp`（Dart AOT）。

## 构建

需要：

* JDK 21 或更高版本（`io.github.libxposed:api:102.0.0` 是 Java 21 字节码，class file version 65）。
* Android SDK Platform 37。
* NDK r30（`30.0.16248370`）。
* CMake `3.22.1`。

在 Windows 上每次调用 Gradle 前设置 `JAVA_HOME`：

```powershell
$env:JAVA_HOME = 'D:\Program Files\Android\Android Studio\jbr'
.\gradlew.bat :app:assembleDebug
.\gradlew.bat :app:lintDebug
```

Debug APK 位于 `app/build/outputs/apk/debug/app-debug.apk`。构建 Release：

```powershell
.\gradlew.bat :app:assembleRelease
```

AGP 会输出 `app/build/outputs/apk/release/app-release-unsigned.apk`。该 APK 未签名，安装前需要用 `apksigner` 签名。R8 会压缩控制面板代码，但 `kotlin-stdlib` 是真实的运行时依赖，不能从 `*RuntimeClasspath` 排除，否则打开面板会触发 `NoClassDefFoundError`。

当前 Release 构建中，arm64-v8a 的 stripped `.so` 约 114 KB（Dobby + 诊断探测），armeabi-v7a 约 51 KB（配置解析器、诊断和静态 C++ 运行时；Dart hook 只在 arm64 上编译）。R8 通过 `app/src/main/keepRules/rules.keep` 保留入口类；`io.github.libxposed` 是 `compileOnly`，不会被打包。

这些打包细节是功能的一部分，改动后很容易引入回归：

* `packaging.jniLibs.useLegacyPackaging = false` 让 `.so` 以未压缩、页对齐的方式存储（AGP 同时在合并后的 Manifest 中生成 `android:extractNativeLibs="false"`）。Vector 的 `VectorModuleClassLoader.findLibrary()` 只对 STORED 条目返回 `base.apk!/lib/<abi>/libmiuihome_edit.so`；被 deflate 的条目对它不可见，`System.loadLibrary()` 会报 `UnsatisfiedLinkError`。
* `-Wl,-z,max-page-size=16384` 让库能在 16 KB 页设备上加载（构建出的 `.so` 每个 `LOAD` 段都按 `0x4000` 对齐）。
* `-Wl,--exclude-libs,ALL` 阻止静态链接的 libc++ 和 Dobby 符号重新导出；模块的 dynsym 只应暴露 `native_init`。
* `targets += "miuihome_edit"` 让 AGP 只构建本模块自己的库；Dobby 的 CMakeLists 还会定义共享目标 `dobby`，没有这一行 AGP 会额外打包一个 `libdobby.so`。
* arm64 下的 `-ffixed-x21 -ffixed-x22 -ffixed-x24 -ffixed-x26 -ffixed-x27 -ffixed-x28` 阻止 C 编译器占用 Dart 的活动 ABI 寄存器；否则替换函数返回或跳回原函数时可能破坏 Dart 状态。x15（Dart 的栈指针）无法用 `-ffixed` 保护——任何 libc 调用都可能改写它——所以每个 hook 都经过一段保存/恢复寄存器的汇编 shim。
* 不要从 `*RuntimeClasspath` 排除 `org.jetbrains.kotlin:kotlin-stdlib`。`MainActivity`、`LauncherConfig`、`ConfigStore` 都是 Kotlin，缺少 stdlib 会直接崩溃。
* 源 Manifest 中不要写 `android:extractNativeLibs`：AGP 已根据 `useLegacyPackaging` 推导，重复声明会产生警告。

## 控制面板

安装并打开 `MiuiHomeEdit`。五个开关互相独立，默认全部关闭。每次切换都会立即持久化保存，但 native 只在 `com.miui.home` 启动时读取一次；因此开关变更要重启桌面才生效，并且在该桌面进程结束前始终使用同一份启动快照：

1. `ConfigStore` 先把 JSON 原子写入模块自己的 `filesDir`。
2. 有 Root 时，通过 `su` 执行一段 POSIX `sh` 脚本，把文件复制到 `com.miui.home` 的设备加密 `files` 目录（`/data/user_de/<user>/...`）、CE 私有 `files` 目录和外部 `files` 目录，并 `chown` 到桌面 UID；同时刷新 `/storage/emulated/<user>/MiuiHomeEdit/`。写入使用“临时文件 + `mv`”，启动器不会读到写了一半的 JSON。设备加密目录在开机解锁前就可读，因此冷启动早期也能拿到开关。
3. 没有 Root 时，在获得“所有文件访问”权限后尝试写公共目录 `MiuiHomeEdit`；该回退路径可能无法被桌面读取，面板状态区会明确提示。
4. 面板不会自动强制停止桌面；单独的“重启 com.miui.home”按钮执行 `am force-stop --user <桌面用户> com.miui.home`。**保存开关不会热更新正在运行的桌面**：只有桌面进程重新启动后，native 才会读取新 JSON 并锁存为本次进程的配置快照。

配置文件名为 `miuihome_edit.json`：

```json
{
  "version": 1,
  "hide_drawer_all_tab": false,
  "hide_edit_button": false,
  "hide_settings_gear": false,
  "hide_search_box": false,
  "hide_swipe_up_arrow": false
}
```

`module_config.cpp` 按以下顺序检查候选路径（`<user>` 由桌面进程 UID 计算：主用户为 `0`，常见工作资料为 `10`）：

| 顺序 | 路径 | 说明 |
| --- | --- | --- |
| 1 | `/data/user_de/<user>/com.miui.home/files/miuihome_edit.json` | 设备加密（DE）路径，由 Root 写入；解锁前即可读，优先用于开机冷启动 |
| 2 | `/data/user/<user>/com.miui.home/files/miuihome_edit.json` | CE 私有路径，由 Root 写入；解锁后可靠 |
| 3 | `/storage/emulated/<user>/Android/data/com.miui.home/files/miuihome_edit.json` | 桌面的外部 files 目录，作为 Root 回退 |
| 4 | `/storage/emulated/<user>/MiuiHomeEdit/miuihome_edit.json` | 无 Root 设备的公共回退 |
| 5 | `/data/local/tmp/miuihome_edit.json` | 仅调试用；SELinux 通常会拒绝应用读取 |

注意：**不是只读第一个存在的文件**。所有可读且 JSON 有效的候选都会参与比较，修改时间（`st_mtim`，纳秒精度）最新的文件获胜。这样当私有路径写入失败时，旧的私有文件不会遮盖刚写好的外部副本；时间戳完全相同时按表中顺序（越靠前越优先）。若没有任何有效文件，五个功能全部关闭。

在 HyperOS 4 + KernelSU 上，Root 域名可能看不到桌面的 `app_data_file` 目录，因此 Root 脚本写 DE/CE 私有路径会失败。桌面进程本身有权限，所以 native 在读到任一有效配置后，会把同一份 JSON **自行写回** `/data/user_de/<user>/com.miui.home/files/miuihome_edit.json`；下一次冷启动优先读取它。日志中的 `config: persisted ...` 表示这一步成功。

`module_config.cpp` 在 `native_init`（以及 `Install()` 的幂等复查）中读取**一次**配置，把五个功能位锁存到进程内原子变量。之后即使面板改写、删除或短暂无法读取任何一个候选文件，运行中的桌面也不会重新读取：hook 始终使用启动时的快照，直到 `com.miui.home` 进程退出。下一次进程启动才会读取最新 JSON，再次锁存。这个“作用域启动时生效、重启前保持不变”的模型与 HyperCeiler 的配置生效方式一致。

启动时如果所有候选都不可读、JSON 损坏或没有任何有效文件，本次进程内五个功能全部关闭；请确认配置写入路径后重启桌面，而不是在运行中反复切换开关。日志中的 `config: snapshot selected ...` 表示快照已锁定，`config: process snapshot latched; changes need a launcher restart` 表示本进程之后不会再读取配置。

## 日志与诊断

五个开关“勾选后没有作用”时，不要只看界面；按下面的顺序采集一次日志即可区分原因。

如果报告停在 `config candidates` 并出现 `/system/bin/sh: no closing quote`，说明是旧版把整段脚本作为单个 `su` 参数被截断，不是模块或作用域的结论。本版本改为先写 `diagnostics.sh` 再执行，并会输出 `===== done =====` 作为完整性标记。


### 控制面板

控制面板底部有三个按钮：

1. **收集诊断日志（需要 Root）**：通过 `su` 读取桌面进程的 native 日志、logcat、配置候选路径、`/proc/<pid>/maps`、包版本和 LSPosed/Vector 日志，生成一份文本报告。
   诊断脚本会先写入模块的 `files/diagnostics.sh`（并尝试写一份外部私有目录副本），再执行 `su -c "sh <file>"`。旧版本把整段脚本作为单个 `su` 参数传递，部分 su 会截断命令，导致报告停在 `/system/bin/sh: no closing quote`，后面的 logcat、framework 日志全部丢失。

2. **复制诊断报告**：把完整报告放到剪贴板。
3. **导出诊断报告…**：通过系统文件选择器保存为 `.txt`，便于发送。

报告保存在模块私有的 `files/diagnostics/`，最多保留最近 5 份。没有 Root 时仍会生成本地部分，但读不到桌面私有目录和完整 logcat。报告包含设备型号、build fingerprint、桌面包版本、进程映射和配置布尔值，不包含桌面数据文件；发送前可自行检查内容。
报告末尾必须有 `===== done =====`。没有该行说明 root 脚本没有执行完，此时“没有 MiuiHomeModule 日志”不能作为模块未加载的结论；先看 `root.exitCode`、`root.script`、`root.scriptBytes` 和 root 输出中的 shell 错误。

报告新增的 `target apk libs` 段直接读取桌面 APK 的 zip 目录，列出 `libapp.so`、`libflutter.so` 和 `lib/arm64-v8a/` 条目。maps/fd 过滤同时匹配 `.apk`：系统 priv-app（本机为 `/product/priv-app/MiuiHome`）把未压缩的 `libapp.so` 直接从 `MiuiHome.apk` 映射，maps 里只出现 APK 路径而没有独立的 `libapp.so` 行。因此脚本不再输出 `mapping: <lib> present/absent`（在这类设备上那只是假阴性），改为打印 `maps note`；引擎库与 hook 的真实状态以 native 日志的 `image:` 与 `hooked <name>` 行为准。
`module apk` 段会打印当前安装 APK 中的 `META-INF/xposed/*` 文件内容和 `lib/` 条目，用来确认 `java_init.list`、`native_init.list` 与 `libmiuihome_edit.so` 确实被打进包里；如果这里缺失，问题在 APK 打包，不在作用域或 Root。
`lsposed database` 段在设备有 `sqlite3` 时直接查询 `modules_config.db` 的 `modules` 与 `scope` 表：如果模块包名不在结果里，说明管理器没有注册/启用它；如果模块在但 scope 没有 `com.miui.home`，说明作用域没选对。




### native 日志位置

`diag.cpp` 会在启动器进程内同时写 logcat 和滚动文件（超过 2 MB 时改名为 `.1` 后新建）；私有目录可写时，另外两处镜像副本与主日志逐行同步写入：

| 顺序 | 路径 | 说明 |
| --- | --- | --- |
| 1 | `/data/user/<user>/com.miui.home/files/miuihome_edit.log` | 首选；桌面进程自己的私有目录 |
| 2 | `/storage/emulated/<user>/Android/data/com.miui.home/files/miuihome_edit.log` | 并行写入的镜像副本；私有目录不可写时作为主日志，su 可读（面板的全文件访问在 Android 11+ 读不到别的包的 `Android/data`） |
| 3 | `/storage/emulated/<user>/MiuiHomeEdit/miuihome_edit.log` | 并行写入的公共镜像副本；面板有全文件访问即可直接读取，无需 Root |
| 4 | `/data/local/tmp/miuihome_edit.log` | 仅调试；SELinux 通常拒绝应用访问 |

`diagnostics: native log is … (mirror: …)` 的 logcat 行会列出本次实际打开的镜像副本（可能为 `none`）。面板“收集诊断”按公共副本 → `Android/data` 副本的顺序直接读取（无需 Root），Root 脚本也会 `ls`/`tail` 这三处以及 `/proc/<pid>/root` 下的同一路径；报告里的 `===== native log (direct read) =====` 段就是直读结果。

日志开头记录模块编译时间、进程名、PID/UID、Android release/API、内核、build fingerprint、指针大小和页大小。随后记录：

* 框架 API 版本、`hookFunc`/`unhookFunc` 地址；
* `native_init` 是否进入目标进程；
* 当前进程已映射的引擎镜像清单（`image:` 行，包含从 APK 内映射、因而在 maps 里看不到的 `libapp.so`）；
* `libapp.so` 每个 `PT_LOAD` 段和 GNU build-id；
* 配置文件每个候选路径的 `stat`/`access` 错误码、大小、mtime、UID/GID；
* JSON 解析结果和五个功能位；
* `dart_hooks.cpp` 中八个注入点（七个名称，含只读诊断用的 `editor-init`）与对象来源的实际配置值（五项功能；`emptyWidgetPool=0x6250`，`falsePool`/`falseVaddr` 保持 `0x0`，因为 `false` 直接由 x22 推导）；
* 打开开关时，对 `libapp.so` 中“全部 / 分类 / 编辑 / 设置 / 搜索 / 上滑”的字符串探测结果，包括字符串对象 class id、length 和可能的对象池引用地址。探测在后台线程执行，避免拖慢桌面启动。

也可以手动查看：

```powershell
adb logcat -s MiuiHomeEdit
adb shell su -c "tail -n 300 /data/user/0/com.miui.home/files/miuihome_edit.log"
```

### 如何读结论

| 日志现象 | 结论 |
| --- | --- |
| 完全没有 `MiuiHomeModule constructed` | 框架没有加载模块：检查模块是否启用、是否支持 API 101+、作用域是否包含 `com.miui.home` |
| 有 `constructed`，没有 `native_init` | `.so` 没有映射：检查 APK 的 `lib/arm64-v8a/`、`native_init.list`、`extractNativeLibs=false` |
| `native_init ... (target)` 和两个镜像基址都有 | 注入成功；继续看下一行 |
| `no target offsets configured` | 所有目标都是 `kUnset`：当前是未填写目标段的构建，开关不会生效 |
| `config: snapshot selected ... bits=...` | 启动配置已锁定；若界面仍无变化，问题在 hook 目标或该 Widget 的构建时机 |
| `config: snapshot selected /data/user_de/...` | 冷启动早期从设备加密目录读到配置；这是开机即生效的预期路径 |
| `config: process snapshot latched` | 本进程已锁定配置；面板之后的修改不会再影响当前桌面，直到下次重启 |
| `drawer-tabbar-view: shifted children from 9 to 8` | TabBarView 缓存的页面 children 已同步左移，首屏应为第一个自定义分类；这是第 1 项（标签/页面错位）的决定性日志 |
| `drawer-all-tab: pruning class=92 len=9` | 标签栏列表仍以“全部”开头，开始堆扫描并修剪；随后应出现 `pruned ... to 8 entries` 和 `list now len=8` |
| `drawer-all-tab: tabbar ... list=... len=9` | 标签栏每次构建看到的共享列表；`len=0` 是尚未填充的副本，`len=9` 表示仍含“全部”并会触发修剪 |
| `drawer-all-tab: list now len=8` | 修剪后回读的实际长度，确认共享列表已变成 N-1 |
| `drawer-all-tab-scan: heap match ... first_f10=0x90e85168` | 堆扫描命中了共享分类列表；`first_id` 是“全部”元素的 id 字段原始 Smi 值（当前 `0x4`，即 id 2），`array_len` 应不小于列表长度 |
| `drawer-all-tab-scan: heap scan finished, N match(es), M pruned` | 本次扫描的命中/修剪数量；`M` 通常等于命中的可写列表数，若为 0 看同段 `no matching list could be pruned` |
| `drawer-all-tab: no matching list could be pruned` | 首元素特征匹配但堆上找不到可写目标；通常是 `kAllCategoryField10` 与当前 `libapp.so` 不符 |
| `drawer-ids: shifted id list=0x... from 9 to 8 entries` | 页面取分类 id 用的 `List<int>` 镜像也以“全部”开头，已删首项 | 
| `drawer-ids: shifted category list=0x... (name at +0x10) from 9 to 8 entries` | 页面分类对象列表已同步删首项；`name at +0xNN` 是“名称”引用字段的偏移，正常为 `+0x10`。这一行决定“编辑”图块是否会打开本页分类 |
| `drawer-all-tab: shifted N id list(s) and M category list(s)` | 本次构建的镜像修剪数量；`M=0` 说明分类对象列表没被识别，编辑图块会退回打开上一个分类 |
| `editor-init: self=... class=3868 category id=N` | 只读诊断：编辑器入口收到的分类 id。与所点页面的分类 id 一致即正确（第 1 页为 1、第 4 页为 4、第 8 页为 8；`0` 表示“全部”） |
| `settings-gear: decide ... enabled=1 empty=0x...` | 齿轮构建函数已调用；`empty=0` 表示空 Widget 池尚未解析成功 |
| `swipe-up-arrow: decide ... enabled=1 value=0x...` | 箭头 predicate 已调用；`value=0` 表示 `Bool.false` 未解析成功 |
| `config: no readable config file` | 配置没有写到桌面可读路径，或 SELinux/UID 不允许；看同段 `candidate ... stat failed/access denied` |
| `Dart probe ... pool-like ref=...` | 已得到 Android 17 构建的候选锚点，可交给 IDA/blutter 继续定位目标 |
| 报告没有 `===== done =====` 或出现 `no closing quote` | 采集脚本没有执行完；这是报告生成问题，不能据此判断模块未加载。重新安装本版本后再收集 |
| 旧报告里出现 `mapping: libapp.so absent` | 该行是假阴性：APK 内未压缩的 .so 在 maps 里显示为 apk 路径 + 文件偏移；以 native 日志的 `image:`/`hooked` 行为准 |
| native 日志只有 `waiting for libflutter.so` | 旧版把 libflutter.so 当作安装前提，而该启动器没有这个库；升级到只等 libapp.so 的版本 |
| `target apk libs` 中没有 `libapp.so`/`libflutter.so` | 该启动器不是 Flutter 构建，Dart hook 方案不适用，需要改用 Java/native hook |
| `root.scriptBytes` 正常但 root 输出有 `Permission denied` | 换用报告中的外部脚本副本，或确认 Root 管理器允许读取模块私有目录 |


### Android 17 适配检查

* 模块使用 `compileSdk/targetSdk 37`、NDK r30 和 16 KB 页链接参数；不要改回低版本 SDK 或压缩 native 库。
* 启动器必须是 64 位进程（`arm64-v8a`）。可用 `adb shell getprop ro.product.cpu.abilist` 和应用报告里的 `abis=` 确认。
* Android 17 的启动器 `libapp.so` 与旧版不共享偏移。必须先采集 build-id/字符串锚点，再针对这一个构建填写目标；不要直接沿用 Android 15/16 的 vaddr。
* 从旧包名 `com.example.miuihomeedit` 升级时，先执行 `gradlew clean` 再构建/安装；增量构建可能把旧类留在 APK 的次级 dex 中，虽然不影响 `java_init.list`，但会干扰排查。

* Android 17 对 `logcat` 和跨应用目录的访问更严格，所以 native 日志优先写桌面自己的 `files/`，由控制面板通过 Root 读取。

## 模块加载流程

1. 框架注入 `com.miui.home`，并为 APK 创建模块类加载器。
2. `java_init.list` 指定 `io.github.afhniuk.miuihomeedit.MiuiHomeModule`，类加载器实例化该类。
3. 静态初始化块调用 `System.loadLibrary("miuihome_edit")`。
4. `VectorModuleClassLoader.findLibrary()` 把它解析为 `base.apk!/lib/arm64-v8a/libmiuihome_edit.so`，链接器完成映射。
5. 框架的 `dlopen` hook 将路径与 `native_init.list` 匹配，找到导出的 `native_init` 并**同步**调用；此时 `loadLibrary` 尚未返回。
6. `native_init()` 把 hook 表（`version`、`hookFunc`、`unhookFunc`）复制到模块中，并返回一个回调。
7. 框架对触发 `native_init` 的这次库加载以及进程内之后的每次 `dlopen` 调用该回调。`TryInstallDartHooks()` 只等 `libapp.so` 映射（该启动器不带 `libflutter.so`，引擎在 `libapp_launcher.so` 里），然后安装一次（`libapp.so` 换 base 时由运行期健康监视重新安装）。`native_init` 末尾还会直接尝试一次，并启动一个常驻的 1 秒 watchdog 轮询 `dl_iterate_phdr`：部分 ROM 的加载器绕过框架的 `dlopen` hook，若只依赖回调，快照被映射时不会收到任何事件；安装完成后同一线程继续做补丁完整性检查。

回调运行时框架仍持有自己的注册表互斥锁，而且调用者的 `dlopen` 尚未返回，因此回调必须很短，并且绝不能自己调用 `dlopen`——嵌套加载会重入互斥锁，导致桌面死锁。Dobby 使用 `Plugin.SymbolResolver=OFF` 构建，所以 `DobbyHook()` 本身也不会 `dlopen`。

## 添加 hook

目标有两类。

* **导出的 C 符号**（JNI shim、引擎 API）：不需要偏移。

  ```cpp
  void *original = HookSymbol(handle, "SomeExportedSymbol",
                              reinterpret_cast<void *>(&MyReplacement));
  ```

* **`libapp.so` 内的 Dart AOT 代码**：`dart_hooks.cpp` 为五项桌面功能（抽屉“全部”标签、编辑按钮、设置齿轮、搜索框、上滑箭头）安装八个 Dobby inline hook（七个名称：`swipe-up-arrow` 有两个注入点，另一个是只读诊断用的 `editor-init`）。目标地址位于该文件顶部。

### Dart AOT ABI 要点

* ARM64 Dart 将对象池放在 **x27（PP）**，当前线程放在 **x26（THR）**，写屏障掩码/基址放在 **x28（HEAP_BITS）**。自 Dart 2.19 起布局如此；x28 **不是** PP。常量通过 `ldr xN, [x27, #imm]` 加载。
* 返回值是**带标签的指针**：Smi 为 `value << 1`，堆对象为 `address | 1`。`false`、`true` 和 `null` 都是堆对象，不是整数。把 `x0` 置为 0 返回的是 Smi 0（`int` 0），**不是** `false`，也不是 `null`。
* 常量快照对象（`SizedBox.shrink()`、`Bool.false`、字符串）通常位于预编译镜像中，不可变，且不会被 GC 移动；复用它们是零分配且安全的，但地址必须正确，使用前要先校验。**本构建是个例外**：`libapp.so` 的镜像里没有这些常量，AOT 把它们序列化进 isolate 的全局对象池，而该池在启动时创建于 Dart 堆上，PP（x27）指向它。所以 `SizedBox.shrink()` 只能用“池槽位 + `mincore()` 页驻留检查”解析（`ResolveFromPool`），不能用镜像范围检查；`false` 则干脆绕开池，直接从 x22 取 `NullObject + 0x30`。

### 七个 hook 的实现方式

每个 hook 由两部分组成：`dart_hooks.cpp` 中的 C++ 决策函数 `Decide*()`，以及 Dobby 实际安装的汇编 shim `shim_*`。决策函数返回 0 表示“照原样执行”，返回其他值则作为 Dart 对象直接交给调用方。

| # | 目标 | 返回类型 | 决策函数返回 |
| --- | --- | --- | --- |
| 1 | 抽屉“全部”分类（两个注入点） | `void`（原函数正常执行） | 标签栏 build：校验 receiver 列表并原地移除“全部”；TabBarView build：把缓存的页面 children 左移一位 |
| 2-4 | 编辑 / 齿轮 / 搜索框构建函数 | Dart `Widget` 对象 | 池槽位 `0x6250` 的 `const SizedBox.shrink()` |
| 5 | 上滑箭头 predicate | Dart `bool` 对象 | 规范 `false`（`NullObject + 0x30`，取自 x22） |

为什么需要 shim：这些目标的第一条指令就是 Dart 序言 `stp x29, x30, [x15, #-0x10]!`，随后是 `mov x29, x15` 和 `sub x15, x15, #N`。x15 是 Dart 的栈指针，而 AAPCS64 把它归为临时寄存器，C 函数以及它调用的 `mincore`、`__android_log_print` 都可能改写它；调用方却依赖它（例如 `_showAllAppsArrowIndicator` 在 `0x1176cac` 用 `str x16, [x15]` 写栈参数）。因此 shim 先保存 x15、x0-x8 和 q0-q7，调用决策函数，然后全部恢复：决策结果为 0 时带着原始参数 `br` 到 Dobby 的 trampoline（等价于透明调用原函数，LR 保持不变），否则把结果放进 `x0` 直接 `ret` 回 Dart 调用方。`x21`/`x22`/`x24`/`x26`/`x27`/`x28` 另由 `-ffixed` 保证不被 C 代码分配。

第 1 项由两个 hook 协作。`_AllAppsCategoryTabBar.build`（`0x14be89c`）的 pre-hook 读取 receiver（x1）的 `+0x0c` 分类列表，检查首元素 `+0x10`；仍为“全部”时扫描 `[heap_base, heap_base + 0x40000000)` 的可读页，按“class id + 长度 + 首元素特征”找到所有匹配列表，在可写页上把元素整体前移一个 4 字节压缩指针，并把 `list_raw + 0x0c` 的长度 Smi 改成 `(N-1) << 1`，最后记录 `g_pruned_category_length`。

`_TabBarViewState.build`（`0x12d12a0`）的 pre-hook 解决页面错位：`_TabBarViewState` 已在 state 原始对象 `+0x1c` 缓存了 N 个页面 children，单改标签不会改变它。决策函数读取该列表的长度 Smi 和数据区，只有长度大于 `g_pruned_category_length` 时才把元素整体左移一位并写回 N-1；同一列表地址和已经是 N-1 的列表都会跳过，因此打开/关闭抽屉或重建不会重复删页。两个 hook 都返回 0，让原函数继续执行。

对于 **Widget 构建函数**，替换函数必须返回真实的 `SizedBox.shrink()` 对象。仅仅“改 PC”跳过原函数并不更安全：调用方仍期待 `x0` 中是有效对象。`ret` 本身就是 PC 改变（PC = LR），但返回值函数必须先把真实的 Dart 对象放进 `x0`。

只有静态返回类型接受 `SizedBox`（`Widget`、`Widget?`、`Object?`）时才能这样做。如果 IDA 显示的是 `PreferredSizeWidget`、sliver、`Tab`、模型对象或 `List<Widget>`，返回 `SizedBox` 会触发运行时类型错误；此时应使用对象池中类型匹配的常量、强制走原函数自己的“空”分支，或在上游移除该项。

对于 **bool predicate**，决策函数返回规范的 `Bool.false` 快照对象。另一种方式是直接修改调用方的 `cbz` / `cbnz`：

```cpp
// 强制 "cbz w0, hidden" 永远跳转。目标在 +/-1 MB 内，也落在 b 的 +/-128 MB imm26 范围内。
const std::uint32_t insn = aot::LoadInstruction(text, offset);
const std::uintptr_t pc = text.address + offset;
const std::uintptr_t target = aot::DecodeConditionalBranchTarget(insn, pc);
aot::PatchInstruction(pc, aot::EncodeUnconditionalBranch(pc, target));
```

### Fail-open 规则

每一步失败都退回到原行为，而不是崩溃：

* 未配置的目标或对象来源只记录一次日志并跳过。
* 目标必须位于 `libapp.so` 的可执行 PT_LOAD 内，并按四字节对齐。Dobby 的 arm64 入口补丁是 16 字节；如果目标前三条指令中出现 `ret`，该目标会被拒绝，避免补丁覆盖下一个函数。
* 对象池位于 Dart 堆而不是 `libapp.so` 中，因此槽位按 `pp + 偏移` 计算，并在解引用**之前**用 `mincore()` 确认目标页已驻留；错误偏移只会产生日志，不会 SIGSEGV。
* 从池中取出的候选对象必须能读到对象头，且 class id 与配置一致（`0` 表示只做可读性检查）。`Bool.false` 不再从池中取，而是由 x22（`NullObject`）加 `0x30` 推导。
* `DobbyHook` 返回错误会记录日志，只跳过该目标，其余目标继续安装。如果 Dobby 返回成功但没有 trampoline，则用 `DobbyDestroy` 回滚补丁。
* 决策函数返回 0 时，shim 会在恢复 x0-x8、q0-q7 和 x15 之后 `br` 到 Dobby 的 trampoline，原函数看到的参数与 SP 和直接调用时完全一致；决策函数返回的都是镜像内常量对象，不会分配 Dart 对象，也不会触发写屏障。

如果某些目标在当前启动器构建上没有对应偏移（`kUnset`），它们只会记录 fail-open 日志而不会修改界面：过期的偏移会导致启动循环，因此本模块只对确切的构建版本填写地址，换版本后必须重新按 build-id 核对。

### C 决策函数不能触碰的寄存器

决策函数使用以下编译选项：

`-ffixed-x21 -ffixed-x22 -ffixed-x24 -ffixed-x26 -ffixed-x27 -ffixed-x28`

这些寄存器分别保存 dispatch table、`NullObject`、code、THR、PP 和 HEAP_BITS。没有这些选项时，足够大的 C 函数可能把其中一个寄存器分配给局部变量，读取对象池或返回 `null`/`false` 时就会看到损坏的 Dart 状态。x19/x20/x23/x25 是普通 callee-saved 寄存器，编译器会自动保存。

x15 是 Dart 的栈指针，AAPCS64 却把它列为临时寄存器；`-ffixed-x15` 也只约束本库，管不住 `mincore()`、`sysconf()` 和 `__android_log_print()`。因此 x15 由 `MIUIHOME_HOOK` 生成的汇编 shim 显式保存和恢复，同时保存 x0-x8 与 q0-q7，使“放行”路径与直接调用原函数等价。

### 查找对象池偏移和对象

在 Android 17 上，先打开任意一个开关并重启桌面，让 `diag.cpp` 执行一次字符串探测。日志会给出每个锚点的 `hit`、`class`、`length` 和 `pool-like ref`（形如 `libapp+0x...`），这些是离线 IDA/blutter 分析的起点。然后把确认后的值填入 `dart_hooks.cpp` 的 `namespace target`，重新构建并再次采集日志，确认出现 `hooked <name>` 而不是 `no target offsets configured`。

分析必须使用设备上**这一份** `libapp.so`。从报告的 `target apk libs` 中找到包含 `lib/arm64-v8a/libapp.so` 的 `apk=` 路径，然后复制到电脑：

```powershell
adb shell su -c "cp '<报告中的 apk 路径>' /sdcard/Download/MiuiHome.apk && chmod 0644 /sdcard/Download/MiuiHome.apk"
adb pull /sdcard/Download/MiuiHome.apk
# 在电脑上解压 lib/arm64-v8a/libapp.so，并用报告里的 build-id 核对同一构建
```


使用 IDA 在 `ldr xN, [x27, #imm]` 上显示的 `#imm` 操作数——那才是 `dart_hooks.cpp` 需要的字节偏移，不是 `pp.txt` 的行号。`pp.txt`（或 IDA 交叉引用）用来确认哪个槽位存放 `SizedBox.shrink()` / `false`，`ldr` 的立即数给出偏移。也可以直接使用对象的 vaddr（未加标签的头部地址）。class id 是可选的，但它能把“地址在镜像内”强化为“这确实是 Box/Bool”。

`aot_scanner.h` 可以查找对象池引用附近的代码：

```cpp
const aot::Span text = aot::FindSegment(dart_image, PF_X);
for (std::uintptr_t load : aot::FindPoolReferences(text, 0x1234)) {
    LOGI("pool ref %p -> function %p", reinterpret_cast<void *>(load),
         reinterpret_cast<void *>(aot::FindFunctionStartBefore(text, load)));
}
```

`FindBranchLinkCallers()` 和 `FindAdrpAddReferences()` 分别回答“谁调用了这里”和“谁引用了这个地址”；当整函数 hook 太粗时，可以用 `PatchInstruction()` 配合 `EncodeUnconditionalBranch()` 翻转条件分支。所有解析都应在 `TryInstallDartHooks()` 中只做一次：扫描 30 MB 的 `.text` 只需几十毫秒。

替换函数必须与目标具有相同的调用约定和返回类型。只在函数入口处 hook（绝不要在指令中间），并在进程整个生命周期内保持 hook。新增 Dart 目标时也应沿用本仓库的写法：C++ 只负责“决定返回什么”，入口由 `MIUIHOME_HOOK` 生成的 shim 承担——它会保存 x15、x0-x8 和 q0-q7，放行时再 `br` 到 Dobby 的 trampoline。

## 验证

```powershell
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb shell cmd package resolve-activity --brief com.miui.home
adb logcat -c
adb shell am force-stop com.miui.home
adb shell input keyevent KEYCODE_HOME
adb logcat -s MiuiHomeEdit
```

正常情况下按顺序看到：

```text
loading libmiuihome_edit.so: sdk=37 release=17 ...
libmiuihome_edit.so loaded
MiuiHomeModule constructed (...)
module loaded into com.miui.home via <framework> <version> (api=102, ...)
diag: process=com.miui.home ... runtime api=37 ...
native_init: framework API v2, process 'com.miui.home' (target)
config: candidate /data/user_de/0/com.miui.home/files/miuihome_edit.json: ... 
config: candidate /data/user/0/com.miui.home/files/miuihome_edit.json: ...
config: snapshot selected /data/user_de/0/com.miui.home/files/miuihome_edit.json (bits=0x...)
config: process snapshot latched; changes need a launcher restart
images (native_init): 1 engine image(s) listed above
image: <apk path>!/lib/arm64-v8a/libapp.so base=0x...
libapp.so mapped (base=0x..., reason=native_init)
diag: libapp.so build-id=...
diag: Dart probe ...
Dart hooks: target config drawer=0x14be89c view=0x12d12a0 edit=0x14c2588 settings=0x14bec50 search=0x149df2c swipe=0x1176bac editor-init=0xefbda0 emptyWidgetPool=0x6250 ...
hooked drawer-all-tab at 0x... (original 0x...)
hooked drawer-tabbar-view at 0x... (original 0x...)
hooked edit-button at 0x... (original 0x...)
hooked settings-gear at 0x... (original 0x...)
hooked search-box at 0x... (original 0x...)
hooked swipe-up-arrow at 0x... (original 0x...)
hooked swipe-up-arrow at 0x... (original 0x...)
hooked editor-init at 0x... (original 0x...)
watchdog: hook health monitor active (1 s interval)
empty widget: resolved pool offset 0x6250 -> 0x... (class 7713)
drawer-tabbar-view: state=0x... list=0x... class=92 len=9
drawer-tabbar-view: shifted children from 9 to 8
drawer-all-tab: tabbar #1 self=0x... enabled=1
drawer-all-tab: list=0x... class=92 len=9 data=0x...
drawer-all-tab: pruning class=92 len=9 list=0x...
drawer-all-tab-scan: heap match #1 list=0x... raw=0x... class=92 len=9 data=0x... array_len=9 first_id=0x4 first_f10=0x90e85168
drawer-all-tab-scan: pruned list=0x... to 8 entries
drawer-all-tab-scan: heap scan finished, 1 match(es), 1 pruned, 44040192 bytes
drawer-all-tab: list now len=8
drawer-ids: shifted id list=0x... from 9 to 8 entries
drawer-ids: shifted category list=0x... (name at +0x10) from 9 to 8 entries
drawer-all-tab: shifted 1 id list(s) and 2 category list(s)
editor-init: self=0x... class=3868 category id=1
settings-gear: decide #1 enabled=1 empty=0x...
swipe-up-arrow: decide #1 enabled=1 value=0x...
```

`image:` 行是“这台设备上到底有哪些引擎库”的唯一可靠答案（`libapp.so` 常常直接映射自 `MiuiHome.apk`，`/proc/maps` 里看不到独立文件）。`libapp.so mapped (... reason=...)` 后面的 `reason` 说明这次安装由哪条路径触发：`native_init`（注入时已映射）、某个 `dlopen` 路径，或 `watchdog`（每秒轮询到的安装路径，通常原因为加载器绕过了框架的 `dlopen` hook）。20 秒内始终没有映射时只打印一次 `watchdog: libapp.so still not mapped after 20 s; will keep checking every second` 和当时的 `image:` 清单，之后仍会继续等待（热注入或极慢启动仍能生效）。

`watchdog: hook health monitor active (1 s interval)` 表示常驻健康监视线程已启动。它正常时不输出任何内容；只有检测到补丁被还原、`libapp.so` 换了 base、或目标入口出现既不是原始 prologue 也不是 Dobby 补丁的代码时，才打印 `health: ...`。看到 `health: libapp.so moved ... reinstalling hooks` 或 `health: <name> ... was restored; re-applying` 说明自愈已生效，不需要重启桌面。

`empty widget: resolved pool offset 0x6250 -> 0x... (class 7713)` 是正常的首次解析；这个构建已配置 class id，因此**不应**再出现旧版的 `no class id configured; resolved class ... by trust` 警告。出现 `empty widget: cached object ... is no longer a usable SizedBox; re-resolving` 表示 isolate 已更换，模块正在自动重新取对象。

`hooked <name>` 应该有八行（七个名称；`swipe-up-arrow` 的两个注入点各一行，另有只读诊断用的 `editor-init`）。只有部分行时，缺的那一行前面会打印 `not a safe 16-byte hook site` 或 `DobbyHook(...) failed`，说明偏移与该设备上的 `libapp.so` 不匹配（版本不同或已更新），该目标被跳过，其余目标照常安装。其中 `drawer-tabbar-view` 缺失时，标签会减一但页面仍多一页；看到这种组合应优先核对 `0x12d12a0`。

如果始终没有 `native_init`，说明 `.so` 没有被映射：检查 ABI（`adb shell getprop ro.product.cpu.abilist`）、APK 中是否确实存在 `lib/arm64-v8a/libmiuihome_edit.so`，以及 `native_init.list` 的文件名是否匹配。如果框架报告模块不可用，检查 `java_init.list` 中的包名是否与类实际包名和 R8 keep 规则一致。

安装新包名后，建议先确认模块作用域和包名：

```powershell
adb shell dumpsys package io.github.afhniuk.miuihomeedit | Select-String "versionName|userId"
```

如果设备上还保留旧包名 `com.example.miuihomeedit` 的模块，请先卸载或停用旧模块；两个 APK 会同时出现在模块管理器中，旧模块仍会按旧包名/旧 native 库注入，容易造成“已勾选但无效”的假象。

## 重命名模块

当前包名是 `io.github.afhniuk.miuihomeedit`。如果以后还要改名，需要同步修改：

* `app/build.gradle.kts` 中的 `namespace` 和 `applicationId`；
* `app/src/main/java/io/github/afhniuk/miuihomeedit/` 与 `app/src/main/kotlin/io/github/afhniuk/miuihomeedit/` 下的包声明和目录；
* `META-INF/xposed/java_init.list`；
* `app/src/main/keepRules/rules.keep`；
* `app/src/test` / `app/src/androidTest` 中的包声明和包名断言。

native 库名 `miuihome_edit` 与包名无关，仍由 `CMakeLists.txt` 的 `add_library(miuihome_edit ...)`、`native_init.list` 和 `System.loadLibrary("miuihome_edit")` 三者共同决定。

Release APK 必须签名后才能安装，例如：

```powershell
apksigner sign --ks my-release.jks --out MiuiHomeEdit-release.apk app\build\outputs\apk\release\app-release-unsigned.apk
```

## 第三方代码

`third_party/dobby` 是 Dobby（Apache-2.0，https://github.com/jmpews/Dobby），提交为 `5dfc8546954ce3b3198132ab13fddb89ee92cdd7`；精确的归档地址在 `UPSTREAM.txt`。构建时只链接 `dobby_static` 目标。

为了在 Android 上用 NDK r30 编译，固定的上游代码带有少量本地修复；这些修改保持很小，便于日后更新 Dobby：

* `common/os_arch_features.h` 不再包含 `platform.h`（循环包含会在 `OSMemory` 声明前使用它）。`make_memory_readable()` 在该头文件中声明、在 `platform-posix.cc` 中定义。
* `platform-posix.cc` 在 `platform.h` 之前包含 `dobby/common.h`。
* `ProcessRuntime.h` 同时保留 `base` 和 `load_address`，Linux 实现改用 `MemRange::start()` 而不是直接访问字段。
* `code-patch-tool-posix.cc` 不再包含不存在的 `core/arch/Cpu.h`；`ClearCache()` 已经由 `dobby_internal.h` 提供。
* `closure_bridge_arm64.asm` 在 Android 上使用 ELF 的 `:lo12:` 语法，而不是 Mach-O 的 `@PAGE` / `@PAGEOFF`。
* Dobby 的 CMakeLists 不再在 Debug 构建中强制打开 `DOBBY_DEBUG`，因此模块的 `-DDOBBY_DEBUG=OFF` 是最终生效的设置。
* Dobby 依赖和 Dart hook 只在 arm64 上启用；上游 ARM32 后端无法用 NDK r30 编译，且目标桌面只运行 64 位进程。
