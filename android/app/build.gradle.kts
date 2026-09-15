plugins {
    id("com.android.application")
}

// Same bundle-id convention as T3K_IOS_BUNDLE_ID in plugin/CMakeLists.txt
// (there: "com.TONE3000.TONE3000", overridable per that CMake cache var's
// own comment for signing under a personal team - no CMake-side involvement
// on Android, so it is just hardcoded here to match).
val t3kApplicationId = "com.TONE3000.TONE3000"

android {
    namespace = t3kApplicationId
    compileSdk = 36
    // Pinned, not "latest": JUCE 9.0.1's vendored Oboe copy (in
    // modules/juce_audio_devices/native/oboe) conflicts with NDK 30's
    // <aaudio/AAudio.h>, which now declares AAudio_FallbackMode /
    // AAudio_StretchMode / AAudioPlaybackParameters / AAudio_DeviceType -
    // duplicate/conflicting redefinitions against Oboe's own compatibility
    // shims for those same symbols in AAudioLoader.h. Confirmed working
    // against this JUCE version; bump only after checking JUCE's vendored
    // Oboe copy against whatever NDK version is newer.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = t3kApplicationId
        // ANDROID_PLATFORM must be >= 29: juce_graphics's Android
        // system-font-matching code (juce_Fonts_android.cpp) fails to
        // *compile* below API 29 (Clang "is unavailable: introduced in
        // Android 29" errors, despite the calls already being wrapped in
        // __builtin_available(android 29, *) guards in JUCE's own source).
        // v1 scope is tablet-only, so a 2019+ (Android 10+) floor is a
        // reasonable trade, but revisit if a lower floor is ever needed.
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_PLATFORM=android-29")
                // Only the Standalone app is meaningful on Android (see
                // T3K_IOS OR T3K_ANDROID gating in plugin/CMakeLists.txt) -
                // restrict Gradle's CMake sub-build to that one target
                // rather than also building VST3/the DSP test suite/etc.
                targets += listOf("TONE3000_Standalone")
            }
        }
    }

    // Points at the repo's own root CMakeLists.txt (JUCE fetch, all the
    // configure-time JUCE patches, add_subdirectory(plugin)) - not a
    // duplicate Android-specific CMake tree. Gradle's externalNativeBuild
    // treats this as a sub-build, auto-injecting
    // -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake plus
    // -DANDROID_ABI/-DANDROID_PLATFORM per abiFilters/minSdk.
    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "4.1.2"
        }
    }

    // JUCE's own Android Java glue (JuceApp/JuceActivity referenced directly
    // in AndroidManifest.xml - no custom Activity subclass needed). Paths
    // confirmed against the actual JUCE 9.0.1 tree fetched into libs/juce by
    // the CMake configure above (differs from JUCE's own module-native
    // top-level dirs by module: juce_core uses both "java" and "javacore",
    // juce_gui_basics uses "java" and "javaopt").
    sourceSets["main"].java.srcDirs(
        "../../libs/juce/modules/juce_core/native/java/app",
        "../../libs/juce/modules/juce_core/native/javacore/app",
        "../../libs/juce/modules/juce_core/native/javacore/init",
        "../../libs/juce/modules/juce_gui_basics/native/java/app",
        "../../libs/juce/modules/juce_gui_basics/native/javaopt/app"
    )

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// Factory presets. Desktop installers lay these down outside the app (see
// plugin/CMakeLists.txt / script/create-pkg.sh); iOS carries them as bundle
// resources (same plugin/CMakeLists.txt, T3K_IOS block). Android has neither
// an installer nor a directly-readable bundle path, so they ride as APK
// assets instead - PresetManager::extractFactoryPresetsFromAssets() (Android
// branch, plugin/src/PresetManager.cpp) copies them out to internal storage
// the first time they're needed.
//
// Registered as an extra assets source dir (resources/factory-presets/*
// lands at the APK assets *root*, not nested under a FactoryPresets/
// subfolder - extractFactoryPresetsFromAssets() lists the asset root
// directly to match) rather than a custom Copy task into src/main/assets:
// AGP's own sourceSets wiring tracks this correctly as a task input/output
// dependency everywhere it matters (merge, lint, ...); an ad hoc Copy task
// writing into the literal src/main/assets tree does not - every consumer
// task would need its own explicit dependsOn (lint's
// generateReleaseLintVitalReportModel included, confirmed the hard way: an
// AGP "implicit dependency" validation failure on a real build with only
// the merge*Assets tasks wired).
android.sourceSets.getByName("main").assets.srcDirs("../../resources/factory-presets")
