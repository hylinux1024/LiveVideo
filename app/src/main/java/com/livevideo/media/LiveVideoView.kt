package com.livevideo.media

import android.content.Context
import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.AttributeSet
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * 零拷贝图传视图:
 *
 *  1. 创建一个 OES 外部纹理, 绑定到 SurfaceTexture
 *  2. 把 SurfaceTexture 包成 Surface 交给 C++ 层的 AMediaCodec
 *  3. 硬件解码器把 YUV 直接写到 GraphicBuffer, 不经 CPU
 *  4. 收到 onFrameAvailable 后, 在 GL 线程上 updateTexImage() 并把纹理画到屏幕
 *  5. 在 GL 线程上叠加 OSD (姿态数据等)
 *
 *  renderMode = RENDERMODE_WHEN_DIRTY: 没有新帧就不画, 节省低端机算力
 */
class LiveVideoView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : GLSurfaceView(context, attrs), GLSurfaceView.Renderer {

    private lateinit var surfaceTexture: SurfaceTexture
    private var oesTextureId: Int = -1
    private var program: Int = 0
    private var aPositionLoc: Int = 0
    private var aTexCoordLoc: Int = 0
    private var uTextureLoc: Int = 0

    // 简易 OSD program (纯色 line, 不依赖纹理)
    private var hudProgram: Int = 0
    private var hudPosLoc: Int = 0
    private var hudColorLoc: Int = 0

    private val vertexBuffer: FloatBuffer
    private val texCoordBuffer: FloatBuffer

    @Volatile private var pendingFrame: Boolean = false
    @Volatile private var frameTimestampNs: Long = 0L
    private val frameLock = Any()

    // GL 线程专用, 不与外部共享, 避免并发写入
    private val glLocalTelemetry = TelemetryData()

    init {
        setEGLContextClientVersion(2)
        // 8 bit RGB / 8 bit Alpha / 16 bit depth —— 视频不需要 depth, 16 bit 即可
        setEGLConfigChooser(8, 8, 8, 8, 16, 0)
        preserveEGLContextOnPause = true
        setRenderer(this)
        renderMode = RENDERMODE_WHEN_DIRTY

        // 全屏四边形 (NDC, 屏幕坐标: -1,-1 在左下, 1,1 在右上)
        val verts = floatArrayOf(
            -1f, -1f,
             1f, -1f,
            -1f,  1f,
             1f,  1f
        )
        // OES 纹理的 UV: (s=0, t=0) 对应图像**左上角** (image 坐标系, 不是 GL 坐标系)
        // 屏幕 NDC 的 -1 (下) 要对应图像的下边 (t=1), 1 (上) 对应图像的上边 (t=0)
        // 所以 Y 要翻一下, 否则视频会上下颠倒
        val texs = floatArrayOf(
            0f, 1f,   // 屏幕左下 -> 图像左下
            1f, 1f,   // 屏幕右下 -> 图像右下
            0f, 0f,   // 屏幕左上 -> 图像左上
            1f, 0f    // 屏幕右上 -> 图像右上
        )
        vertexBuffer = ByteBuffer.allocateDirect(verts.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(verts); position(0) }
        texCoordBuffer = ByteBuffer.allocateDirect(texs.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(texs); position(0) }
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        // 1. 编译 shader
        program = createProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
        aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")
        uTextureLoc  = GLES20.glGetUniformLocation(program, "uTexture")

        hudProgram = createProgram(HUD_VS, HUD_FS)
        hudPosLoc   = GLES20.glGetAttribLocation(hudProgram,  "aPosition")
        hudColorLoc = GLES20.glGetUniformLocation(hudProgram, "uColor")

        // 2. 创建 OES 纹理
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        oesTextureId = textures[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S,     GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T,     GLES20.GL_CLAMP_TO_EDGE)

        // 3. 把 OES 纹理绑到 SurfaceTexture
        surfaceTexture = SurfaceTexture(oesTextureId)
        surfaceTexture.setOnFrameAvailableListener {
            synchronized(frameLock) {
                pendingFrame = true
                frameTimestampNs = it.timestamp
            }
            requestRender()
        }

        // 4. 把 Surface 交给 C++ 层, 初始化 AMediaCodec
        val surface = Surface(surfaceTexture)
        // 视频尺寸由 ffmpeg 推流端决定, 这里给一个与推流一致的初始值 (320x240 baseline)
        // 实际分辨率以码流 SPS 为准, 解码器会通过 Output format changed 回调报出真实尺寸
        LiveVideoEngineJNI.initDecoder("video/avc", 320, 240, surface)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        GLES20.glClearColor(0f, 0f, 0f, 1f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

        // 1. 拉取最新一帧到 OES 纹理, 并把对应的 PTS 一起取出来
        //    (lock 内同时清 pendingFrame 和读 timestamp, 避免与 OnFrameAvailable 回调产生 race)
        var currentTsNs: Long = frameTimestampNs
        synchronized(frameLock) {
            if (pendingFrame) {
                surfaceTexture.updateTexImage()
                pendingFrame = false
                currentTsNs = frameTimestampNs
            }
        }

        // 2. 绘制 OES 视频帧
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
        GLES20.glUniform1i(uTextureLoc, 0)

        vertexBuffer.position(0)
        GLES20.glEnableVertexAttribArray(aPositionLoc)
        GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 0, vertexBuffer)

        texCoordBuffer.position(0)
        GLES20.glEnableVertexAttribArray(aTexCoordLoc)
        GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 0, texCoordBuffer)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

        GLES20.glDisableVertexAttribArray(aPositionLoc)
        GLES20.glDisableVertexAttribArray(aTexCoordLoc)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, 0)

        // 3. HUD 叠层: 等真实遥测数据接入后再开, 现在只是占位假数据
        //    注释掉避免污染视频画面
        // LiveVideoEngineJNI.queryTelemetry(currentTsNs / 1000L, glLocalTelemetry)
        // drawHud(glLocalTelemetry, osdProjectionMatrix())
    }

    /**
     * 兼容接口: 把遥测快照透传给 native, 由 native 持有最新值,
     * 下一帧 GL 线程 queryTelemetry 时可同步拿到.
     * 任意线程调用均可.
     */
    fun updateTelemetry(t: TelemetryData) {
        // 直接转发给 native, 避免和 GL 线程竞争 Kotlin 端的对象
        LiveVideoEngineJNI.injectTelemetry(t)
    }

    // ============================================================
    // OSD: 画一个简易 HUD (姿态球框 + 文本)
    // 由于我们只画线段/矩形/字符, 这里使用 GL_LINES + GLES20 内置方法
    // ============================================================
    private fun osdProjectionMatrix(): FloatArray {
        // 单位矩阵 (因为视频已铺满 NDC)
        return floatArrayOf(
            1f, 0f, 0f, 0f,
            0f, 1f, 0f, 0f,
            0f, 0f, 1f, 0f,
            0f, 0f, 0f, 1f
        )
    }

    private fun drawHud(t: TelemetryData, proj: FloatArray) {
        GLES20.glUseProgram(hudProgram)

        val cx = 0f
        val cy = 0f
        val r  = 0.12f
        val segs = 48
        GLES20.glLineWidth(2f)

        // 1) 姿态球外圈
        val pts = FloatArray(segs * 2)
        for (i in 0 until segs) {
            val a = (i.toDouble() / segs) * 2.0 * Math.PI
            pts[i * 2]     = (cx + r * Math.cos(a)).toFloat()
            pts[i * 2 + 1] = (cy + r * Math.sin(a)).toFloat()
        }
        val circle = ByteBuffer.allocateDirect(pts.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(pts); position(0) }
        GLES20.glUniform4f(hudColorLoc, 0f, 1f, 0.5f, 0.9f)
        GLES20.glEnableVertexAttribArray(hudPosLoc)
        circle.position(0)
        GLES20.glVertexAttribPointer(hudPosLoc, 2, GLES20.GL_FLOAT, false, 0, circle)
        GLES20.glDrawArrays(GLES20.GL_LINE_LOOP, 0, segs)

        // 2) pitch 横线 (随俯仰角上下移动)
        val pitchPx = (t.pitch / 90.0).toFloat() * r
        val horizon = floatArrayOf(
            cx - r * 0.9f, cy + pitchPx,
            cx + r * 0.9f, cy + pitchPx
        )
        val horBuf = ByteBuffer.allocateDirect(horizon.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(horizon); position(0) }
        GLES20.glUniform4f(hudColorLoc, 1f, 0.5f, 0.2f, 0.9f)
        horBuf.position(0)
        GLES20.glVertexAttribPointer(hudPosLoc, 2, GLES20.GL_FLOAT, false, 0, horBuf)
        GLES20.glDrawArrays(GLES20.GL_LINES, 0, 2)

        // 3) 电量条 (顶部居中)
        val batWidth = 0.3f
        val batX = -batWidth / 2f
        val batY = 0.85f
        val batFrac = (t.batteryLevel.coerceIn(0, 100)) / 100f
        val bar = floatArrayOf(
            batX,           batY,            batX + batWidth,  batY,
            batX + batWidth, batY,            batX + batWidth,  batY + 0.02f,
            batX + batWidth, batY + 0.02f,    batX,             batY + 0.02f,
            batX,            batY + 0.02f,    batX,             batY,
            batX,            batY,            batX + batWidth * batFrac, batY,
            batX + batWidth * batFrac, batY,   batX + batWidth * batFrac, batY + 0.02f
        )
        val barBuf = ByteBuffer.allocateDirect(bar.size * 4)
            .order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(bar); position(0) }
        GLES20.glUniform4f(hudColorLoc, 0.2f, 1f, 0.2f, 0.95f)
        barBuf.position(0)
        GLES20.glVertexAttribPointer(hudPosLoc, 2, GLES20.GL_FLOAT, false, 0, barBuf)
        GLES20.glDrawArrays(GLES20.GL_LINES, 0, bar.size / 2)

        GLES20.glDisableVertexAttribArray(hudPosLoc)
    }

    // ============================================================
    // Shader 编译
    // ============================================================
    private fun createProgram(vs: String, fs: String): Int {
        val v = compileShader(GLES20.GL_VERTEX_SHADER, vs)
        val f = compileShader(GLES20.GL_FRAGMENT_SHADER, fs)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, v)
        GLES20.glAttachShader(p, f)
        GLES20.glLinkProgram(p)
        val status = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "Program link failed: ${GLES20.glGetProgramInfoLog(p)}")
            GLES20.glDeleteProgram(p)
            return 0
        }
        return p
    }

    private fun compileShader(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val status = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "Shader compile failed: ${GLES20.glGetShaderInfoLog(s)}")
            GLES20.glDeleteShader(s)
            return 0
        }
        return s
    }

    companion object {
        private const val TAG = "LiveVideoView"

        // OES 纹理顶点 shader —— 只需要把顶点位置和纹理坐标透传
        private val VERTEX_SHADER = """
            attribute vec2 aPosition;
            attribute vec2 aTexCoord;
            varying   vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vTexCoord   = aTexCoord;
            }
        """.trimIndent()

        // OES 纹理片段 shader —— 颜色空间转换由 GPU 完成 (EXT_YUV_target 等可省略)
        // 这里直接采样 OES 纹理, 由驱动内部做 YUV->RGB 转换
        private val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform   samplerExternalOES uTexture;
            varying   vec2 vTexCoord;
            void main() {
                gl_FragColor = texture2D(uTexture, vTexCoord);
            }
        """.trimIndent()

        // OSD 用: 纯色 line, 不采样纹理
        private val HUD_VS = """
            attribute vec2 aPosition;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
            }
        """.trimIndent()

        private val HUD_FS = """
            precision mediump float;
            uniform vec4 uColor;
            void main() {
                gl_FragColor = uColor;
            }
        """.trimIndent()
    }
}
