package io.github.afhniuk.miuihomeedit;

import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.util.Log;

import java.util.Arrays;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam;

/**
 * The module's Java entry point.
 *
 * <p>This class exists only because the framework refuses to load an APK without one: Vector's
 * {@code FileSystem.loadModule()} returns {@code ModuleLoad.Unusable} when {@code
 * META-INF/xposed/java_init.list} names no class or when the APK carries no {@code classes*.dex}.
 * There is no dex-less native-only module, in Vector or in LSPosed.
 *
 * <p>The class therefore does one thing: it drags {@code libmiuihome_edit.so} into the process.
 * Every hook lives in {@code main.cpp}.
 *
 * <p>Why {@code System.loadLibrary()} works here: the framework instantiates this class through
 * its own {@code VectorModuleClassLoader}, which overrides {@code findLibrary()} and resolves
 * {@code libmiuihome_edit.so} to {@code base.apk!/lib/<abi>/libmiuihome_edit.so} for entries that
 * are stored uncompressed (see {@code packaging.jniLibs.useLegacyPackaging} in build.gradle.kts).
 * {@code native_init()} then runs synchronously inside that {@code dlopen}, before this class has
 * finished initialising.
 */
public final class MiuiHomeModule extends XposedModule {
    private static final String TAG = "MiuiHomeEdit";

    static {
        try {
            Log.i(TAG, "loading libmiuihome_edit.so: sdk=" + Build.VERSION.SDK_INT
                    + " release=" + Build.VERSION.RELEASE
                    + " abis=" + Arrays.toString(Build.SUPPORTED_ABIS)
                    + " pageSize=" + android.system.Os.sysconf(
                    android.system.OsConstants._SC_PAGESIZE));
            // Must match META-INF/xposed/native_init.list and the CMake target name.
            System.loadLibrary("miuihome_edit");
            Log.i(TAG, "libmiuihome_edit.so loaded");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "libmiuihome_edit.so could not be loaded: check the ABI filters, "
                    + "useLegacyPackaging = false and META-INF/xposed/native_init.list", e);
            throw e;
        }
    }

    public MiuiHomeModule() {
        Log.i(TAG, "MiuiHomeModule constructed (classLoader="
                + getClass().getClassLoader() + ")");
    }

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        Log.i(TAG, "module loaded into " + param.getProcessName()
                + " via " + getFrameworkName() + " " + getFrameworkVersion()
                + " (api=" + getApiVersion()
                + ", frameworkCode=" + getFrameworkVersionCode()
                + ", sdk=" + Build.VERSION.SDK_INT
                + ", release=" + Build.VERSION.RELEASE + ")");
        try {
            ApplicationInfo info = getModuleApplicationInfo();
            if (info != null) {
                Log.i(TAG, "module apk=" + info.sourceDir
                        + " uid=" + info.uid
                        + " dataDir=" + info.dataDir);
            }
        } catch (Throwable error) {
            Log.w(TAG, "could not read module application info", error);
        }
    }
}
