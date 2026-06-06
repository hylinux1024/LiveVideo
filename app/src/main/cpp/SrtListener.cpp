// SrtListener.cpp
// SRT (Secure Reliable Transport) 监听器, 跑在独立 native 线程上
// 收包后直接通过 JNI 回调 DroneEngineJNI.feedStream, 不需要 Kotlin 端做接收线程
#include <jni.h>
#include <android/log.h>
// 头文件原始路径是 srt/srt.h, 但我们走 in-tree 集成,
// 用 srtcore/ 作为 include 路径, 所以这里用 "srt.h"
#include "srt.h"
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

#define SRT_LOG_TAG "SrtListener"
#define SRT_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  SRT_LOG_TAG, __VA_ARGS__)
#define SRT_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SRT_LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_jvm_srt = nullptr;
jclass  g_engine_class = nullptr;   // com.drone.media.DroneEngineJNI
jmethodID g_feedStream_mid = nullptr;

void ensureJniBindings(JNIEnv* env) {
    if (g_engine_class != nullptr) return;
    jclass cls = env->FindClass("com/drone/media/DroneEngineJNI");
    if (cls == nullptr) {
        SRT_LOGE("DroneEngineJNI class not found (wrong classloader?)");
        return;
    }
    g_engine_class = (jclass)env->NewGlobalRef(cls);
    // feedStream(ByteArray, long)V — Kotlin 用 @JvmStatic 后是真正的 static
    g_feedStream_mid = env->GetStaticMethodID(cls, "feedStream", "([BJ)V");
    env->DeleteLocalRef(cls);
    if (g_feedStream_mid == nullptr) {
        SRT_LOGE("feedStream method not found");
    } else {
        SRT_LOGI("JNI bindings cached: DroneEngineJNI.feedStream([B J)V");
    }
}

// 在 native 线程回调 Kotlin 端 feedStream
void dispatchPacket(const uint8_t* data, int len, int64_t pts) {
    static std::atomic<int> pktCount{0};
    static std::atomic<int> totalBytes{0};
    int n = pktCount.fetch_add(1) + 1;
    totalBytes.fetch_add(len);
    if (n == 1 || n == 100 || n % 500 == 0) {
        char hex[32] = {0};
        int dump = std::min(8, len);
        for (int i = 0; i < dump; ++i) {
            snprintf(hex + i*3, 4, "%02x ", data[i]);
        }
        bool isTs = (len > 0 && data[0] == 0x47);
        SRT_LOGI("pkt #%d: %d bytes, head=[%s] %s",
                 n, len, hex,
                 isTs ? "<-- MPEG-TS detected, demuxer will NOT find H.264 NALUs" : "");
    }
    if (g_jvm_srt == nullptr || g_feedStream_mid == nullptr) return;
    JNIEnv* env = nullptr;
    if (g_jvm_srt->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_jvm_srt->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return;
        }
    }
    jbyteArray arr = env->NewByteArray(len);
    if (arr == nullptr) return;
    env->SetByteArrayRegion(arr, 0, len, reinterpret_cast<const jbyte*>(data));
    env->CallStaticVoidMethod(g_engine_class, g_feedStream_mid, arr, (jlong)pts);
    env->DeleteLocalRef(arr);
    // 注意: GetEnv 拿到的不需要 Detach, AttachCurrentThread 拿到的才需要
    // 这里简化处理, 短任务不主动 detach, 由 JVM 在线程退出时清理
}

class SrtListenerImpl {
public:
    bool start(int port) {
        if (running_.exchange(true)) return true;
        port_ = port;
        thread_ = std::thread([this]{ runLoop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (sock_ != SRT_INVALID_SOCK) {
            srt_close(sock_);
            sock_ = SRT_INVALID_SOCK;
        }
        if (thread_.joinable()) thread_.join();
    }

    ~SrtListenerImpl() { stop(); }

private:
    std::thread       thread_;
    std::atomic<bool> running_{false};
    SRTSOCKET         sock_ = SRT_INVALID_SOCK;
    int               port_ = 1234;

    void runLoop() {
        if (g_jvm_srt == nullptr) {
            SRT_LOGE("JavaVM not set, aborting");
            return;
        }
        JNIEnv* env = nullptr;
        if (g_jvm_srt->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            SRT_LOGE("AttachCurrentThread failed");
            return;
        }
        // 主线程上已经预绑定了 (SrtBridge.init -> nativeInitBindings)
        // 这里 double-check: 如果 SRT 线程比 SrtBridge.init 启动得还早, 还是需要补一次
        if (g_engine_class == nullptr) {
            SRT_LOGI("JNI bindings not pre-set, binding on SRT thread (may fail if classloader is bootstrap)");
            ensureJniBindings(env);
        }

        sock_ = srt_create_socket();
        if (sock_ == SRT_INVALID_SOCK) {
            SRT_LOGE("srt_create_socket failed");
            return;
        }

        // SO_REUSEADDR: app 强杀后端口可能还在 TIME_WAIT, 不开这个 bind 会失败
        int reuse = 1;
        srt_setsockopt(sock_, 0, SRTO_REUSEADDR, &reuse, sizeof(reuse));

        // SRT 低延迟参数:
        // - latency = 120ms (业内图传常见值, 抗 30% 丢包仍有低延迟)
        // - maxbw = 0  (不限速, 实时)
        // - payloadsize = 1316 (单包 <= MTU, 跟 UDP 对齐)
        // - rcvbuf / sndbuf = 12MB
        // - lossmaxttl = 30 (30 跳后丢弃未到达的包)
        int latency_ms = 120;
        srt_setsockopt(sock_, 0, SRTO_LATENCY,    &latency_ms, sizeof(latency_ms));
        int tsbpd = 0;  // 关闭 TSBPD, 不用 SRT 内部时间戳缓冲, 更低延迟
        srt_setsockopt(sock_, 0, SRTO_TSBPDMODE,  &tsbpd,     sizeof(tsbpd));
        srt_setsockopt(sock_, 0, SRTO_RCVLATENCY, &latency_ms, sizeof(latency_ms));
        int payload = 1316;
        srt_setsockopt(sock_, 0, SRTO_PAYLOADSIZE, &payload,   sizeof(payload));
        int rcvbuf = 12 * 1024 * 1024;
        srt_setsockopt(sock_, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        int lossmax = 30;
        srt_setsockopt(sock_, 0, SRTO_LOSSMAXTTL, &lossmax,    sizeof(lossmax));
        int overhead = 25;
        srt_setsockopt(sock_, 0, SRTO_OHEADBW,    &overhead,   sizeof(overhead));
        srt_setsockopt(sock_, 0, SRTO_UDP_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        srt_setsockopt(sock_, 0, SRTO_UDP_SNDBUF, &rcvbuf, sizeof(rcvbuf));
        int64_t maxbw = 0;  // 0 = 不限速
        srt_setsockopt(sock_, 0, SRTO_MAXBW, &maxbw, sizeof(maxbw));

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(port_);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (srt_bind(sock_, (sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
            SRT_LOGE("srt_bind port %d failed: %s", port_, srt_getlasterror_str());
            srt_close(sock_);
            sock_ = SRT_INVALID_SOCK;
            return;
        }
        if (srt_listen(sock_, 5) == SRT_ERROR) {
            SRT_LOGE("srt_listen failed: %s", srt_getlasterror_str());
            srt_close(sock_);
            sock_ = SRT_INVALID_SOCK;
            return;
        }
        SRT_LOGI("SRT listening on 0.0.0.0:%d (latency=%dms)", port_, latency_ms);

        while (running_.load()) {
            sockaddr_in client{};
            int addrlen = sizeof(client);
            SRTSOCKET client_sock = srt_accept(sock_, (sockaddr*)&client, &addrlen);
            if (client_sock == SRT_INVALID_SOCK) {
                if (running_.load()) {
                    SRT_LOGE("srt_accept error: %s", srt_getlasterror_str());
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            char ip[64]{}; inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
            SRT_LOGI("Client connected: %s:%d", ip, ntohs(client.sin_port));
            handleClient(client_sock);
            srt_close(client_sock);
            SRT_LOGI("Client disconnected");
        }

        srt_close(sock_);
        sock_ = SRT_INVALID_SOCK;
        g_jvm_srt->DetachCurrentThread();
        SRT_LOGI("SRT loop exited");
    }

    void handleClient(SRTSOCKET client_sock) {
        // srt_recvmsg 第二个参数是 char*, 不是 uint8_t*
        std::vector<char> buf(64 * 1024);
        while (running_.load()) {
            int n = srt_recvmsg(client_sock, buf.data(), (int)buf.size());
            if (n == SRT_ERROR) {
                int err = srt_getlasterror(nullptr);
                if (err == SRT_EASYNCRCV || err == SRT_EINVSOCK) break;
                SRT_LOGE("srt_recvmsg error: %s", srt_getlasterror_str());
                break;
            }
            if (n <= 0) continue;
            // n 是 SRT 负载长度, 直接 push 到 native 解码队列
            // pts 设为 0, 让 C++ 内部按 30FPS 累加
            dispatchPacket(reinterpret_cast<const uint8_t*>(buf.data()), n, 0LL);
        }
    }
};

SrtListenerImpl* g_listener = nullptr;

} // namespace

// ============================================================
// JNI 导出
// ============================================================
extern "C" {

// 由 DroneMediaEngine.cpp 的 JNI_OnLoad 调用, 把 JavaVM 传过来并初始化 libsrt
// 注: 必须用 JNIEXPORT (默认 visibility=default) 否则被 -fvisibility=hidden
// 隐藏后, 另一个 .cpp 文件的 JNI_OnLoad 引用不到, 会导致 undefined symbol
__attribute__((visibility("default"))) void initSrtSubsystem(JavaVM* vm) {
    g_jvm_srt = vm;
    if (srt_startup() != 0) {
        SRT_LOGE("srt_startup failed");
        return;
    }
    SRT_LOGI("SRT subsystem initialized");
}

__attribute__((visibility("default"))) void cleanupSrtSubsystem() {
    srt_cleanup();
}

// 在主线程上做 JNI 绑定, 缓存 class 和 methodID
// 必须在 Kotlin `object` 加载完成后调用 (SrtBridge.init 时机刚好)
JNIEXPORT void JNICALL
Java_com_drone_media_SrtBridge_nativeInitBindings(JNIEnv* env, jclass) {
    ensureJniBindings(env);
}

JNIEXPORT void JNICALL
Java_com_drone_media_SrtBridge_nativeStart(JNIEnv* /*env*/, jclass, jint port) {
    if (g_listener == nullptr) g_listener = new SrtListenerImpl();
    g_listener->start((int)port);
}

JNIEXPORT void JNICALL
Java_com_drone_media_SrtBridge_nativeStop(JNIEnv* /*env*/, jclass) {
    if (g_listener != nullptr) g_listener->stop();
}

} // extern "C"
