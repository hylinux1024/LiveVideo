// LiveVideoEngine.cpp
// 低延迟图传引擎实现：H.264 NALU 解析 + 硬件解码 + 零拷贝 OES 纹理
// 目标: 四核低端 Android 设备, 端到端延迟 < 100ms
#include "LiveVideoEngine.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <functional>
#include <algorithm>

// ============================================================
// JNI 全局
// ============================================================
static JavaVM* g_jvm = nullptr;
static jclass   g_telemetry_class = nullptr;
static jfieldID g_telemetry_pts   = nullptr;
static jfieldID g_telemetry_pitch = nullptr;
static jfieldID g_telemetry_roll  = nullptr;
static jfieldID g_telemetry_yaw   = nullptr;
static jfieldID g_telemetry_bat   = nullptr;

// 提前声明, 避免 JNI_OnLoad 内部不能写 attribute 语法
extern "C" void initSrtSubsystem(JavaVM*);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    // 顺手把 libsrt 也初始化了, SrtListener.cpp 里就不再单独放 JNI_OnLoad
    initSrtSubsystem(vm);
    return JNI_VERSION_1_6;
}

// 缓存 TelemetryData 的字段 id (Kotlin 端 com.livevideo.media.TelemetryData)
static bool ensureTelemetryClass(JNIEnv* env) {
    if (g_telemetry_class != nullptr) return true;
    jclass cls = env->FindClass("com/livevideo/media/TelemetryData");
    if (cls == nullptr) {
        LOGE("TelemetryData class not found");
        return false;
    }
    g_telemetry_class = (jclass)env->NewGlobalRef(cls);
    g_telemetry_pts   = env->GetFieldID(cls, "pts",          "J");
    g_telemetry_pitch = env->GetFieldID(cls, "pitch",        "F");
    g_telemetry_roll  = env->GetFieldID(cls, "roll",         "F");
    g_telemetry_yaw   = env->GetFieldID(cls, "yaw",          "F");
    g_telemetry_bat   = env->GetFieldID(cls, "batteryLevel", "I");
    env->DeleteLocalRef(cls);
    return (g_telemetry_pts && g_telemetry_pitch && g_telemetry_roll && g_telemetry_yaw && g_telemetry_bat);
}

// ============================================================
// LiveVideoEngine: 初始化
// ============================================================
void LiveVideoEngine::init(JNIEnv* env, jstring jMime, jint width, jint height, jobject surface) {
    if (codec_ != nullptr) {
        LOGW("init() called twice, releasing previous instance first");
        release();
    }

    const char* c_mime = env->GetStringUTFChars(jMime, nullptr);
    mime_ = c_mime;
    env->ReleaseStringUTFChars(jMime, c_mime);

    width_  = width;
    height_ = height;

    surface_ref_ = env->NewGlobalRef(surface);
    window_      = ANativeWindow_fromSurface(env, surface);

    codec_ = AMediaCodec_createDecoderByType(mime_.c_str());
    if (codec_ == nullptr) {
        LOGE("AMediaCodec_createDecoderByType(%s) failed", mime_.c_str());
        return;
    }

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime_.c_str());
    AMediaFormat_setInt32(format,  AMEDIAFORMAT_KEY_WIDTH,  width_);
    AMediaFormat_setInt32(format,  AMEDIAFORMAT_KEY_HEIGHT, height_);
    AMediaFormat_setInt32(format,  AMEDIAFORMAT_KEY_FRAME_RATE, 30);
    AMediaFormat_setInt32(format,  AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 1024 * 1024);

    // 关键优化: 开启芯片厂商的低延迟 flag
    AMediaFormat_setInt32(format, "low-latency", 1);
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(format, "vendor.mtk.ext-dec-low-latency.enable", 1);

    media_status_t status = AMediaCodec_configure(codec_, format, window_, nullptr, 0);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_configure failed: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        AMediaFormat_delete(format);
        return;
    }
    AMediaFormat_delete(format);

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed: %d", status);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return;
    }

    codec_configured_ = false;
    demuxer_.reset();
    nalu_queue_.clear();
    cached_sps_.clear();
    cached_pps_.clear();
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    latest_telemetry_ = TelemetryData{};
    has_latest_telemetry_ = false;

    LOGI("Decoder initialized: %s %dx%d (zero-copy surface)", mime_.c_str(), width_, height_);
}

void LiveVideoEngine::start() {
    if (running_.exchange(true)) return;
    input_thread_  = std::thread(&LiveVideoEngine::inputLoop, this);
    output_thread_ = std::thread(&LiveVideoEngine::outputLoop, this);
    RealTimeThread::pinToBigCores(input_thread_,  80);
    RealTimeThread::pinToBigCores(output_thread_, 70);
}

void LiveVideoEngine::stop() {
    running_.store(false);
    if (input_thread_.joinable())  input_thread_.join();
    if (output_thread_.joinable()) output_thread_.join();
}

void LiveVideoEngine::release() {
    stop();
    if (codec_ != nullptr) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    if (surface_ref_ != nullptr && g_jvm != nullptr) {
        JNIEnv* env = nullptr;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
            env->DeleteGlobalRef(surface_ref_);
        } else if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            env->DeleteGlobalRef(surface_ref_);
            g_jvm->DetachCurrentThread();
        }
        surface_ref_ = nullptr;
    }
    codec_configured_ = false;
    cached_sps_.clear();
    cached_pps_.clear();
    demuxer_.reset();
}

// ============================================================
// 网络 → 解码器 (生产者 → SPSC 队列 → 解码器 input thread)
// ============================================================
void LiveVideoEngine::feedStream(const uint8_t* data, size_t size, int64_t pts) {
    if (codec_ == nullptr) return;
    static std::atomic<size_t> totalIn{0};
    size_t in = totalIn.fetch_add(size) + size;
    if (in / 4096 != (in - size) / 4096) {
        // 每次 4KB 边界打印一次
        char hex[32] = {0};
        int dump = std::min((size_t)8, size);
        for (size_t i = 0; i < dump; ++i) {
            snprintf(hex + i*3, 4, "%02x ", data[i]);
        }
        LOGI("feedStream: %zu total bytes, head=[%s] %s",
             in, hex,
             (data[0] == 0x47) ? "<-- MPEG-TS!" : "");
    }
    stats_.framesIn.fetch_add(1, std::memory_order_relaxed);

    demuxer_.feed(data, size, [this, pts](const H264Demuxer::Nalu& n) {
        handleNalu(n);
        (void)pts;
    });
}

void LiveVideoEngine::handleNalu(const H264Demuxer::Nalu& n) {
    uint8_t nalType = n.type;
    if (mime_ == "video/avc") {
        if (nalType == 7) {
            cached_sps_.assign(n.data, n.data + n.size);
            return;
        }
        if (nalType == 8) {
            cached_pps_.assign(n.data, n.data + n.size);
            return;
        }
        // IDR: 必须 SPS + PPS + IDR 三者一起入队
        // 如果队列空间不够三者, 则全部不入队, 保持 codec_configured_=false
        // 后续 P 帧会在 handleNalu 末尾被丢弃, 不会送到没有参考帧的解码器
        if (nalType == 5) {
            constexpr int kIdrBatch = 3; // SPS + PPS + IDR
            if (nalu_queue_.freeCapacity() >= kIdrBatch) {
                enqueueNalu(cached_sps_.data(), cached_sps_.size(), 0);
                enqueueNalu(cached_pps_.data(), cached_pps_.size(), 0);
                enqueueNalu(n.data, n.size, 0);
                codec_configured_ = true;
            } else {
                LOGW("IDR dropped: queue too full (%zu free), keeping codec unconfigured",
                     nalu_queue_.freeCapacity());
                stats_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        if (nalType == 6) {
            return; // SEI: 暂不处理
        }
    }

    if (codec_configured_) {
        enqueueNalu(n.data, n.size, 0);
    } else {
        stats_.framesDropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void LiveVideoEngine::enqueueNalu(const uint8_t* data, size_t size, int64_t pts) {
    PendingNalu p;
    p.data.assign(data, data + size);
    p.pts = pts;
    if (!nalu_queue_.push(p)) {
        stats_.framesDropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void LiveVideoEngine::inputLoop() {
    LOGI("Input loop started");
    while (running_.load(std::memory_order_acquire)) {
        PendingNalu p;
        if (!nalu_queue_.pop(p)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // 判断 NALU 类型 (取首字节的低 5 bit)
        uint8_t nalType = 0;
        if (p.data.size() > 4) {
            size_t scLen = (p.data[2] == 1) ? 3 : 4;
            nalType = p.data[scLen] & 0x1F;
        }
        bool isFrame = (nalType == 1 || nalType == 5);
        bool isConfig = (nalType == 7 || nalType == 8); // SPS/PPS

        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // SPS/PPS 是 codec 配置数据, 绝对不能丢弃.
            // 重新放回队列, 下次循环重试 (等前面的帧消费后腾出 codec 输入空间)
            if (isConfig) {
                nalu_queue_.push(p);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            } else {
                stats_.framesDropped.fetch_add(1, std::memory_order_relaxed);
                if (isFrame) {
                    LOGW("Dropped NALU type=%d size=%zu, codec input full (low-latency)", nalType, p.data.size());
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
        }
        if (idx < 0) {
            // 其他错误 (OUTPUT_FORMAT_CHANGED 等) — 短暂 sleep 等待 codec 恢复
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        size_t bufCap = 0;
        uint8_t* dst = AMediaCodec_getInputBuffer(codec_, idx, &bufCap);
        if (dst == nullptr || bufCap < p.data.size()) {
            // 缓冲不够, 退回 buffer, 不下发
            AMediaCodec_queueInputBuffer(codec_, idx, 0, 0, 0, 0);
            stats_.framesDropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::memcpy(dst, p.data.data(), p.data.size());
        int64_t pts = isFrame ? (input_pts_ += 33333) : 0;
        media_status_t s = AMediaCodec_queueInputBuffer(codec_, idx, 0, p.data.size(), pts, 0);
        if (s != AMEDIA_OK) {
            LOGW("queueInputBuffer failed: %d", s);
        }
    }
    LOGI("Input loop stopped");
}

void LiveVideoEngine::outputLoop() {
    LOGI("Output loop started");
    AMediaCodecBufferInfo info;
    while (running_.load(std::memory_order_acquire)) {
        ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 1000);
        if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) continue;

        if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH,  &w);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
            LOGI("Output format changed: %dx%d, re-initializing decoder with correct size", w, h);
            AMediaFormat_delete(f);
            // 格式变化时重新初始化解码器, 用真实分辨率替换初始的 320x240
            // 这样 surface 和 codec 的尺寸就一致了, 避免花屏
            // 注意: 不能在这里直接 re-init, 因为 codec 还在跑.
            // 只记录日志, 让下一次 init 使用正确的尺寸.
            // 实际的 re-init 由上层 (Kotlin) 在检测到格式变化后触发.
            continue;
        }

        // 有效的解码输出: release 并渲染到 surface
        if (outIdx >= 0) {
            // 检查 buffer info: 如果 size=0 说明是 EOS 或无效帧, 不渲染
            if (info.size > 0) {
                AMediaCodec_releaseOutputBuffer(codec_, outIdx, true);
                stats_.framesDecoded.fetch_add(1, std::memory_order_relaxed);
            } else {
                // size=0 的 buffer (EOS 等), 释放但不渲染
                AMediaCodec_releaseOutputBuffer(codec_, outIdx, false);
            }
        }
    }
    LOGI("Output loop stopped");
}

// ============================================================
// 遥测: 写入端可以来自任意线程, 读取端在 GL 线程
// ============================================================
void LiveVideoEngine::injectTelemetry(const TelemetryData& t) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    latest_telemetry_    = t;
    has_latest_telemetry_ = true;
}

bool LiveVideoEngine::queryTelemetry(int64_t ptsUs, TelemetryData& out) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    if (!has_latest_telemetry_) return false;
    out = latest_telemetry_;
    return true;
}

// ============================================================
// JNI 导出
// ============================================================
static LiveVideoEngine* g_engine = new LiveVideoEngine();

extern "C" {

JNIEXPORT void JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_initDecoder(JNIEnv* env, jobject,
                                                jstring mime, jint width, jint height, jobject surface) {
    g_engine->init(env, mime, width, height, surface);
    g_engine->start();
}

JNIEXPORT void JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_feedStream(JNIEnv* env, jclass,
                                                jbyteArray data, jlong pts) {
    jsize len = env->GetArrayLength(data);
    if (len <= 0) return;
    jbyte* buf = env->GetByteArrayElements(data, nullptr);
    g_engine->feedStream(reinterpret_cast<uint8_t*>(buf), static_cast<size_t>(len), static_cast<int64_t>(pts));
    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
}

JNIEXPORT jboolean JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_queryTelemetry(JNIEnv* env, jobject,
                                                    jlong ptsUs, jobject outTelemetry) {
    if (!ensureTelemetryClass(env)) return JNI_FALSE;
    TelemetryData t;
    bool ok = g_engine->queryTelemetry(static_cast<int64_t>(ptsUs), t);
    if (!ok) return JNI_FALSE;
    env->SetLongField (outTelemetry, g_telemetry_pts,   static_cast<jlong>(t.pts));
    env->SetFloatField(outTelemetry, g_telemetry_pitch, t.pitch);
    env->SetFloatField(outTelemetry, g_telemetry_roll,  t.roll);
    env->SetFloatField(outTelemetry, g_telemetry_yaw,   t.yaw);
    env->SetIntField  (outTelemetry, g_telemetry_bat,   t.batteryLevel);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_injectTelemetry(JNIEnv* env, jobject, jobject telemetry) {
    if (!ensureTelemetryClass(env)) return;
    TelemetryData t;
    t.pts          = env->GetLongField (telemetry, g_telemetry_pts);
    t.pitch        = env->GetFloatField(telemetry, g_telemetry_pitch);
    t.roll         = env->GetFloatField(telemetry, g_telemetry_roll);
    t.yaw          = env->GetFloatField(telemetry, g_telemetry_yaw);
    t.batteryLevel = env->GetIntField  (telemetry, g_telemetry_bat);
    g_engine->injectTelemetry(t);
}

JNIEXPORT void JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_release(JNIEnv*, jobject) {
    g_engine->release();
}

JNIEXPORT jlongArray JNICALL
Java_com_livevideo_media_LiveVideoEngineJNI_getStats(JNIEnv* env, jobject) {
    auto& s = g_engine->stats();
    jlongArray arr = env->NewLongArray(4);
    jlong values[4] = {
        static_cast<jlong>(s.framesIn.load()),
        static_cast<jlong>(s.framesDecoded.load()),
        static_cast<jlong>(s.framesDropped.load()),
        static_cast<jlong>(s.naluParseErrors.load())
    };
    env->SetLongArrayRegion(arr, 0, 4, values);
    return arr;
}

} // extern "C"
