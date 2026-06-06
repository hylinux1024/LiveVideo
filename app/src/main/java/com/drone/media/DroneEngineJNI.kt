package com.drone.media

/**
 * 硬件解码器 + 零拷贝渲染 JNI 桥接
 * 实际的解码、解析、输出线程全部在 native 层完成, Kotlin 只负责
 *  - 推 H.264 原始字节流 (来自 UDP / OTG / 文件)
 *  - 注入遥测 (任意线程)
 *  - 在 GL 线程上同步查询遥测
 *  - 读取统计
 */
object DroneEngineJNI {
    init {
        System.loadLibrary("dronemedia")
    }

    /**
     * 初始化硬件解码器
     * @param mime   视频 MIME, 常用 "video/avc" (H.264) 或 "video/hevc" (H.265)
     * @param width  视频宽 (码流中)
     * @param height 视频高
     * @param surface OES 纹理绑定的 Surface (由 GL 线程创建并持有)
     */
    external fun initDecoder(mime: String, width: Int, height: Int, surface: android.view.Surface)

    /**
     * 推一帧 H.264/H.265 原始字节流
     * 解码器内部会切 NALU, 缓存 SPS/PPS, 并自动在 IDR 前下发 codec config
     * @param data 一帧 (或一段网络包) 的原始字节
     * @param pts  PTS, 单位 us, 不传时由 native 层按 30FPS 自动递增
     *
     * 注: 必须 @JvmStatic, native 侧 SrtListener.cpp 用 GetStaticMethodID + CallStaticVoidMethod
     * 调用, 否则 JNI 拿不到 methodID. Kotlin `object` 默认生成的是实例方法, 加 @JvmStatic
     * 后才是真正的 JVM 静态方法.
     */
    @JvmStatic
    external fun feedStream(data: ByteArray, pts: Long = 0L)

    /**
     * 在 GL 线程调用, 把当前画面 PTS 传给 native, 查回最近一条遥测
     * @return true 表示查到了 (outTelemetry 已被填充)
     */
    external fun queryTelemetry(ptsUs: Long, outTelemetry: TelemetryData): Boolean

    /**
     * 写入最新遥测快照 (任意线程)
     * OSD 绘制时, native 会返回最新一次注入的遥测
     */
    external fun injectTelemetry(telemetry: TelemetryData)

    /** 释放所有 native 资源, Activity.onDestroy 时调用 */
    external fun release()

    /**
     * 读取统计 [in, decoded, dropped, parseErrors]
     * - in:        送入 native 的总字节帧数
     * - decoded:   解码器成功输出帧数
     * - dropped:   主动丢弃(队列满/未配置)的帧数
     * - parseErrors: NALU 解析错误次数
     */
    external fun getStats(): LongArray
}
