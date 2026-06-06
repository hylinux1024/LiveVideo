plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.drone.media"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.drone.media"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        // 低端四核机型只需要这两个 ABI 即可，覆盖 99% 的设备
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }

        // 把 ABI 的最小 CPU 特性设到 Cortex-A53，保证 VFP/ASIMD 一定可用
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-26"
                )
                cFlags += listOf("-O2", "-fvisibility=hidden", "-fno-rtti", "-fno-exceptions")
                cppFlags += listOf("-O2", "-fvisibility=hidden", "-fno-rtti", "-fno-exceptions",
                                   "-std=c++17", "-Wno-unused-parameter")
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
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // release 不打调试符号，减小体积
            ndk {
                debugSymbolLevel = "none"
            }
        }
        debug {
            isDebuggable = true
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        resources {
            excludes += listOf("/META-INF/{AL2.0,LGPL2.1}", "META-INF/DEPENDENCIES")
        }
    }
}

kotlin {
    jvmToolchain(17)
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.ktx)
    implementation(libs.androidx.constraintlayout)
    implementation(libs.androidx.lifecycle.runtime.ktx)

    testImplementation(libs.junit)
}
