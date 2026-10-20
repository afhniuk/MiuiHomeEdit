package io.github.afhniuk.miuihomeedit

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticScriptTest {
    private fun build(
        privateDir: String = "/data/user/0/com.miui.home/files",
        deviceDir: String = "/data/user_de/0/com.miui.home/files",
        publicDir: String = "/storage/emulated/0/MiuiHomeEdit",
    ): String = DiagnosticScript.build(
        modulePackage = "io.github.afhniuk.miuihomeedit",
        targetPackage = "com.miui.home",
        configFile = "miuihome_edit.json",
        privateDir = privateDir,
        deviceDir = deviceDir,
        externalDir = "/storage/emulated/0/Android/data/com.miui.home/files",
        publicDir = publicDir,
        userId = 0,
    )

    @Test
    fun scriptContainsEveryDiagnosticSectionAndTerminator() {
        val script = build()

        listOf(
            "===== script =====",
            "===== identity =====",
            "===== properties =====",
            "===== packages =====",
            "===== launcher processes =====",
            "===== config candidates =====",
            "===== native logs =====",
            "===== logcat =====",
            "===== framework logs =====",
            "===== done =====",
        ).forEach { section ->
            assertTrue("missing $section", script.contains(section))
        }
    }

    @Test
    fun placeholdersAndTemplateMarkersAreGone() {
        val script = build()

        assertFalse(script.contains("§"))
        assertFalse(script.contains("{{"))
        assertFalse(script.contains("}}"))
        assertTrue(script.contains("P_DIR='/data/user/0/com.miui.home/files'"))
        assertTrue(script.contains("for pkg in 'io.github.afhniuk.miuihomeedit' 'com.miui.home'"))
        assertTrue(script.contains("com.miui.home|com.miui.home:*)"))
    }

    @Test
    fun scriptUsesUnixLineEndings() {
        assertFalse(build().contains('\r'))
    }

    @Test
    fun everyLineHasBalancedShellQuotes() {
        build().lineSequence().forEachIndexed { index, line ->
            assertEquals("unbalanced single quotes on line ${index + 1}: $line",
                0, line.count { it == '\'' } % 2)
            assertEquals("unbalanced double quotes on line ${index + 1}: $line",
                0, line.count { it == '"' } % 2)
        }
    }

    @Test
    fun configCandidatesJoinVariablesInsteadOfNestingQuotes() {
        val script = build()

        assertTrue(script.contains("CONFIG=miuihome_edit.json"))
        assertTrue(script.contains("for f in \"\$P_DIR/\$CONFIG\" \"\$D_DIR/\$CONFIG\" " +
            "\"\$E_DIR/\$CONFIG\" \"\$PUB_DIR/\$CONFIG\" \"/data/local/tmp/\$CONFIG\""))
        assertTrue(script.contains("D_DIR='/data/user_de/0/com.miui.home/files'"))
        assertFalse("a shell quote was injected inside a double-quoted path",
            script.contains("'miuihome_edit.json'\""))
    }

    @Test
    fun launcherMapsSectionAlsoMatchesApkBackedLibraries() {
        val script = build()

        assertTrue(script.contains("grep -E 'libapp|libflutter|libmiuihome_edit|libdobby|\\.apk'"))
        // The old per-library "mapping: <lib> present/absent" lines reported
        // "absent" for libraries stored inside the APK, which sent the analysis
        // in the wrong direction on HyperOS 4. They must not come back.
        assertTrue(script.contains("maps note: APK-embedded libs appear as their .apk path"))
        assertFalse(script.contains("mapping: libapp.so absent"))
    }

    @Test
    fun nativeLogsAreAlsoReachableThroughTheLauncherRoot() {
        val script = build()

        assertTrue(script.contains("\"/proc/\$pid/root\$P_DIR\"/miuihome_edit.log*"))
        // The public mirror written by diag.cpp is readable with all-files
        // access on every Android version, so the script collects it too.
        assertTrue(script.contains("\"\$PUB_DIR\"/miuihome_edit.log*"))
        // $P_DIR is a shell variable whose value is already quoted at assignment,
        // so this expands to /proc/<pid>/root/data/user/0/<pkg>/files/<log>*.
        assertTrue(script.contains("P_DIR='/data/user/0/com.miui.home/files'"))
        assertTrue(script.contains("ls -laZ \"\$d\""))
    }

    @Test
    fun pathsWithSpacesStayQuoted() {
        val script = build(publicDir = "/storage/emulated/0/My Files/MiuiHomeEdit")

        assertTrue(script.contains("PUB_DIR='/storage/emulated/0/My Files/MiuiHomeEdit'"))
    }
}
