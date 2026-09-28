package com.example.wifidirect

import android.app.Application
import android.os.Build

/**
 * Application 入口。除了对外暴露 [instance] 让 transfer 包的成员在没有
 * Activity 上下文时仍能拿到 application context 之外，没有别的状态。
 *
 * 之前的版本里这里有 `getBluetoothUuid()`，每次装机生成一个随机 UUID 写到
 * SharedPreferences —— 但这个函数从未被任何代码调用，且 `BluetoothManager.APP_UUID`
 * 是一个独立的硬编码 UUID。两条不一致的 UUID 路径在调试上是个坑，所以把
 * `getBluetoothUuid()` 直接删了；所有调用方统一从 [com.example.wifidirect.transfer.Constants.BLUETOOTH_APP_UUID]
 * 取 UUID。
 */
class App : Application() {

    companion object {
        @JvmStatic
        lateinit var instance: App
            private set

        fun api(): Int = Build.VERSION.SDK_INT
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
    }
}
