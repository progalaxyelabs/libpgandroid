plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.pgandroid.test"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.pgandroid.test"
        minSdk = 29       // Android 10 — required by pgandroid (shm_open)
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
        }
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // jniLibs/ in the app module bundles libpgandroid.so for tests that
    // exercise the native library directly. The symlink points to out/.
    sourceSets["main"].jniLibs.srcDirs("src/main/jniLibs")

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    // pgandroid library module (Kotlin wrapper + JNI bridge)
    implementation(project(":pgandroid"))

    // Instrumented test dependencies
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.1")
    androidTestImplementation("androidx.test:rules:1.6.0")
    androidTestImplementation("junit:junit:4.13.2")
}
