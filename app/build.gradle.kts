plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.wifidirect"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.example.wifidirect"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    // 使用 Java 17 toolchain，避免 JDK 21 编译 source/target 8 时一堆废弃警告。
    // Gradle 会自动下载 / 选择合适的 JDK；本地 JDK 21 仍然能驱动构建本身。
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = true
    }

    // Release 包：
    // - 从 gradle.properties（或环境变量）读取签名信息；缺失时回退到 debug signing
    //   保证 CI 上没配置签名密钥时仍能构建。
    // - 启用 R8 minify + 资源压缩，体积约可缩减一半以上。
    signingConfigs {
        create("release") {
            val storeFilePath = providers.gradleProperty("wfdReleaseStoreFile").orNull
                ?: System.getenv("WFD_RELEASE_STORE_FILE")
            if (!storeFilePath.isNullOrBlank()) {
                storeFile = file(storeFilePath)
                storePassword = providers.gradleProperty("wfdReleaseStorePassword").orNull
                    ?: System.getenv("WFD_RELEASE_STORE_PASSWORD") ?: ""
                keyAlias = providers.gradleProperty("wfdReleaseKeyAlias").orNull
                    ?: System.getenv("WFD_RELEASE_KEY_ALIAS") ?: ""
                keyPassword = providers.gradleProperty("wfdReleaseKeyPassword").orNull
                    ?: System.getenv("WFD_RELEASE_KEY_PASSWORD") ?: ""
            } else {
                // 没配置就退回到 debug signing，至少能让 release assemble 跑通
                initWith(signingConfigs.getByName("debug"))
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
        debug {
            isMinifyEnabled = false
        }
    }}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("androidx.lifecycle:lifecycle-service:2.8.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.7.3")
    testImplementation("org.robolectric:robolectric:4.11.1")
    testImplementation("androidx.test:core:1.6.1")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
}
