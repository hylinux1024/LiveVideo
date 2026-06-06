package com.livevideo.media

import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * 图传主界面:
 *  1. 默认启动 SRT 接收模式: native 侧 srt_listen + srt_recvmsg, 收包后通过 JNI 回调 feedStream
 *  2. 也可切到 UDP 模式 (调试用, SRT 才是生产级)
 *  3. 把字节流通过 JNI 推给 native 硬件解码器
 *  4. native 层解码后通过 SurfaceTexture → OES 纹理 → GLSurfaceView 上屏
 *  5. 主线程 HUD 文本 (TextView) 定期更新姿态数据 (这里用 sin/cos 模拟)
 */
class MainActivity : AppCompatActivity() {

    private lateinit var videoView: LiveVideoView
    private lateinit var statsText: TextView

    private val ui = Handler(Looper.getMainLooper())

    // 默认 SRT 模式 (生产级图传标准, 抗丢包, 延迟可调)
    // 按钮可在 SRT / UDP 两种模式间切换
    private var mode = Mode.SRT
    private var udpSource: UdpVideoSource? = null
    private lateinit var modeButton: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        videoView    = findViewById(R.id.droneVideoView)
        statsText    = findViewById(R.id.statsText)
        modeButton   = findViewById(R.id.modeButton)

        // 初始按钮文案与默认模式一致
        modeButton.text = "Mode: ${mode.label}"

        // 等 GL 线程 ready 后, 显示统计角标
        videoView.queueEvent {
            ui.post {
                statsText.visibility = View.VISIBLE
            }
        }

        // 默认启动 SRT 接收
        startFeedThread()

        // 模式切换按钮
        modeButton.setOnClickListener { toggleMode() }

        // 周期性更新统计 (主线程)
        ui.postDelayed(statsUpdater, 100)
    }

    /**
     * 在 SRT / UDP 两种模式之间循环切换.
     */
    private fun toggleMode() {
        // 停掉当前源
        udpSource?.stop()
        udpSource = null
        try { SrtBridge.nativeStop() } catch (_: Throwable) {}
        // 循环: SRT -> UDP -> SRT
        mode = when (mode) {
            Mode.SRT -> Mode.UDP
            Mode.UDP -> Mode.SRT
        }
        modeButton.text = "Mode: ${mode.label}"
        startFeedThread()
    }

    override fun onResume() {
        super.onResume()
        videoView.onResume()
    }

    override fun onPause() {
        super.onPause()
        videoView.onPause()
    }

    override fun onDestroy() {
        // 关键: 主动 stop SRT, 让 ffmpeg 端能立刻看到断开, 走它的重连逻辑
        // 不然 OS 强杀时 ffmpeg 那边要等 TCP keepalive 超时才知道挂了
        try { SrtBridge.nativeStop() } catch (_: Throwable) {}
        try {
            LiveVideoEngineJNI.release()
        } catch (t: Throwable) {
            Log.e(TAG, "release failed", t)
        }
        ui.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    /**
     * 启动"网络接收"线程
     *  - SRT: native 侧 srt_listen + srt_recvmsg 循环, 收包后通过 JNI 回调 feedStream
     *  - UDP: 阻塞监听 UDP 1236, ffmpeg 用 udp:// 推流. 仅在 NALU <= MTU 时正常.
     */
    private fun startFeedThread() {
        when (mode) {
            Mode.SRT -> startSrtSource()
            Mode.UDP -> startUdpSource()
        }
    }

    private fun startSrtSource() {
        Log.i(TAG, "Switching to SRT listen mode on :$SRT_PORT")
        SrtBridge.nativeStart(SRT_PORT)
    }

    private fun startUdpSource() {
        Log.i(TAG, "Switching to UDP receive mode on :$UDP_PORT")
        val src = UdpVideoSource(UDP_PORT) { bytes, _ ->
            LiveVideoEngineJNI.feedStream(bytes, 0L)
        }
        udpSource = src
        src.start()
    }

    // ============================================================
    // 统计角标刷新 (主线程)
    // FPS = (本次 DEC 增量) / (本次时间增量), 滑动窗口
    // ============================================================
    private var lastDecCount: Long = 0
    private var lastUpdateNs: Long = 0L
    private val statsUpdater = object : Runnable {
        override fun run() {
            try {
                val stats = LiveVideoEngineJNI.getStats()
                val inb   = stats[0]
                val dec   = stats[1]
                val drop  = stats[2]
                val nowNs = System.nanoTime()
                val fps = if (lastUpdateNs > 0L && nowNs > lastUpdateNs) {
                    (dec - lastDecCount).toDouble() * 1_000_000_000.0 / (nowNs - lastUpdateNs)
                } else 0.0
                lastDecCount = dec
                lastUpdateNs = nowNs
                statsText.text = "FPS %4.1f\nIN  %d\nDEC %d\nDROP %d".format(fps, inb, dec, drop)
            } catch (_: Throwable) {}
            ui.postDelayed(this, 200)
        }
    }

    companion object {
        private const val TAG = "MainActivity"
        private const val SRT_PORT = 1234
        private const val UDP_PORT = 1236
    }

    private enum class Mode(val label: String) {
        SRT("SRT"),
        UDP("UDP")
    }
}
