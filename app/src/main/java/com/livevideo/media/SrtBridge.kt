package com.livevideo.media

/**
 * SRT (Secure Reliable Transport) 桥接
 *
 * SRT 是 UDP 之上的可靠传输, 直播/广电领域主流协议 (Haivision 开源).
 * 优势: 自带 FEC + 加密 + ARQ, 抗 30%+ 丢包, 延迟可调 (20-1000ms).
 *
 * ffmpeg 推流命令:
 *   ffmpeg -re -i test.mp4 \
 *     -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
 *     -g 30 -bf 0 -tune zerolatency -preset ultrafast \
 *     -an -f mpegts \
 *     srt://<phone-ip>:1234?mode=listener&latency=80
 */
object SrtBridge {
    init {
        System.loadLibrary("livevideo")
        // 必须在主线程上做 JNI 绑定: SRT 的 native 线程 bootstrap classloader 找不到 app 类
        nativeInitBindings()
    }

    /**
     * 一次性 JNI 绑定: 在主线程上 FindClass("LiveVideoEngineJNI") + GetStaticMethodID("feedStream"),
     * 缓存成全局引用. 后续 SRT 线程直接拿缓存, 不再 FindClass.
     */
    private external fun nativeInitBindings()

    /** 启动 SRT 监听, 在 native 线程 accept/recv, 收到后直接回调 LiveVideoEngineJNI.feedStream */
    external fun nativeStart(port: Int)

    /** 停止 SRT 监听 (会 close socket, ffmpeg 端会立即看到 disconnect, 走它的 retry 逻辑) */
    external fun nativeStop()
}
