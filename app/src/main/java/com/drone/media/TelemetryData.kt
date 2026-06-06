package com.drone.media

/**
 * 遥测数据 (与 C++ 端 TelemetryData 字段一一对应)
 * 通过 JNI 在 native 层读写; 字段名必须与 C++ 端 GetFieldID 保持一致
 */
data class TelemetryData(
    var pts: Long = 0L,
    var pitch: Float = 0f,
    var roll: Float = 0f,
    var yaw: Float = 0f,
    var batteryLevel: Int = 0
)
