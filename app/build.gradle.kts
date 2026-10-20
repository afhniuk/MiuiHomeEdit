plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "io.github.afhniuk.miuihomeedit"
    compileSdk {
        version = release(37)
    }
    // Same toolchain Vector itself is built with; NDK r30 produces 16 KB-page
    // compatible libraries by default.
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "io.github.afhniuk.miuihomeedit"
        minSdk = 35
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"



        ndk {
            // HyperOS ships the launcher as a 64-bit process with a 32-bit
            // variant; nothing else can ever load this module.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                // Only the module's own shared library is needed. Dobby's
                // CMakeLists also defines a shared libdobby target, but AGP
                // would build it (and package libdobby.so) if it built "all".
                targets += "miuihome_edit"
                arguments += listOf(
                    // One runtime, statically linked: there is no libc++_shared.so
                    // to package and nothing to resolve across the module/launcher
                    // boundary.
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    // Android 15+ kernels may use 16 KB pages. Vector passes the
                    // same flag; an unaligned module .so fails to map on them.
                    "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            optimization {
                enable = true
                packageScope = setOf("androidx.**", "kotlin.**", "kotlinx.**")
            }
        }
    }

    compileOptions {
        // io.github.libxposed:api:102.0.0 is a Java 21 artefact (class file
        // version 65), so javac must be able to read v65 classes.
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    lint {
        // The launcher ships only ARM processes; an x86 build could never be
        // loaded into com.miui.home, so ChromeOS support does not apply.
        disable += "ChromeOsAbiSupport"
    }

    packaging {
        jniLibs {
            // REQUIRED, not cosmetic. VectorModuleClassLoader.findLibrary() only
            // returns "base.apk!/lib/<abi>/libx.so" when the zip entry is STORED;
            // compressed (legacy packaging) entries are skipped and
            // System.loadLibrary() then fails with UnsatisfiedLinkError.
            useLegacyPackaging = false
        }
    }
}

dependencies {
    // Supplied by Vector/LSPosed inside the injected process. compileOnly keeps
    // it out of the APK; the framework refuses modules that bundle their own copy.
    compileOnly(libs.libxposed.api)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}

// The control panel is written in Kotlin. Unlike the previous native-only
// build, kotlin-stdlib is now a real runtime dependency and must stay in the
// APK; do not exclude it from *RuntimeClasspath.
