package com.livevideo.media

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.SocketException

/**
 * UDP 视频源: 在独立线程上阻塞读取 DatagramSocket, 收到包后回调 onPacket.
 *
 * 配套 ffmpeg 命令:
 *   ffmpeg -re -i test.mp4 -c:v copy -f h264 -flush_packets 1 -pkt_size 1316 \
 *          udp://<Android-IP>:1234
 *
 * 注意事项:
 *  - 不要用 DatagramSocket(null).apply { bind(InetSocketAddress(port)) } 这种写法,
 *    Kotlin 编译器会把 apply 块里的 `port` 解析到 DatagramSocket.getPort() (返回 -1),
 *    而不是外层 UdpVideoSource.port, 导致 IllegalArgumentException: port out of range:-1.
 *    正确做法是 DatagramSocket(port) 直接构造.
 */
class UdpVideoSource(
    private val port: Int = 1234,
    private val onPacket: (ByteArray, Int) -> Unit
) {
    @Volatile private var running = false
    private var socket: DatagramSocket? = null
    private var thread: Thread? = null

    fun start() {
        if (running) return
        if (port !in 1..65535) {
            Log.e(TAG, "Invalid port: $port, refuse to start")
            return
        }
        running = true
        thread = Thread({ runLoop() }, "DroneUdpRx").also { it.start() }
    }

    fun stop() {
        running = false
        try { socket?.close() } catch (_: Throwable) {}
        thread?.join(500)
        thread = null
        socket = null
    }

    private fun runLoop() {
        Log.i(TAG, "runLoop starting, port=$port")
        val rcvBuf = ByteArray(64 * 1024)
        val sock: DatagramSocket = try {
            // 直接用 (port) 构造, 一步到位; 不要拆成 DatagramSocket(null)+bind
            DatagramSocket(port).apply {
                reuseAddress = true
                receiveBufferSize = 4 * 1024 * 1024
            }
        } catch (e: SocketException) {
            Log.e(TAG, "Bind port $port failed: ${e.message}")
            return
        } catch (e: Exception) {
            Log.e(TAG, "Socket init failed", e)
            return
        }
        socket = sock
        Log.i(TAG, "UDP listening on 0.0.0.0:$port (rcvBuf=${sock.receiveBufferSize})")

        while (running) {
            val pkt = DatagramPacket(rcvBuf, rcvBuf.size)
            try {
                sock.receive(pkt)
            } catch (e: SocketException) {
                break // close() 触发, 正常退出
            } catch (e: Exception) {
                Log.w(TAG, "receive error", e)
                continue
            }
            if (pkt.length <= 0) continue
            // pkt.data 会被下一次 receive 复用, 这里必须拷贝
            val copy = ByteArray(pkt.length)
            System.arraycopy(pkt.data, 0, copy, 0, pkt.length)
            try {
                onPacket(copy, pkt.length)
            } catch (e: Throwable) {
                Log.w(TAG, "onPacket callback failed", e)
            }
        }
        Log.i(TAG, "UDP loop exited")
    }

    companion object {
        private const val TAG = "UdpVideoSource"
    }
}
