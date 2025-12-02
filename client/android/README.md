# Android 客户端骨架

这是 mi_client 的 Android 占位实现，基于 Jetpack Compose/Kotlin，后续将接入 KCP/聊天/媒体。
构建步骤：
1) 安装 Android Studio/SDK (compileSdk=34, JDK17)。
2) 在 core/client/android 运行 `./gradlew assembleDebug`。
3) 待接入核心逻辑后替换 MainActivity 与网络层。
