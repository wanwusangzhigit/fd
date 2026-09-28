# =================================================-----------
# WiFi Direct File Transfer — ProGuard / R8 规则
# =================================================-----------

# --- 通用 AndroidX 规则（proguard-android-optimize.txt 已含大部分）---

# ViewBinding 生成的 *Binding 类通过反射调用 findViewById；保留它们的公共 API。
-keep class **_Binding { *; }

# 保持 R 文件字段（资源 ID）以便运行时反射访问。
-keepclassmembers class **.R$* { <fields>; }

# --- 应用代码 ---
# 项目自身代码可以放心混淆（没有用到反射序列化）。但如果将来引入了反射注册
# （例如基于 Class.forName 的插件机制），需要在下面补 -keep。

# 保持 TransferEvent sealed 家族与 Transport 枚举：TransferBus 的 when 分支
# 在反射意义上不依赖它们，但 kotlinx.coroutines flow 会把这些对象写到
# StackTrace 里，混淆后崩溃栈不可读。
-keep enum com.example.wifidirect.transfer.Transport { *; }
-keep class com.example.wifidirect.transfer.TransferEvent$* { *; }

# 保持 Constants 中的常量被 native / JNI 反射时仍可见。
-keep class com.example.wifidirect.transfer.Constants { *; }

# --- 第三方库 ---
# kotlinx.coroutines：默认规则已包含；无需手动添加。
