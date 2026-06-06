// DroneMediaEngine.h
// 低延迟图传引擎头文件：H.264 NALU 解析 + 硬件解码 + 零拷贝 OES 纹理
#pragma once

#include <jni.h>
#include <android/native_window.h>
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <mutex>
#include <functional>

// ============================================================
// 宏 / 日志
// ============================================================
#ifndef LOG_TAG
#define LOG_TAG "DroneMediaEngine"
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================
// 遥测数据 (PTS 单位为微秒 us)
// 严格 packed+aligned(4) 避免 CPU 缓存行 false sharing
// ============================================================
struct __attribute__((packed, aligned(4))) TelemetryData {
    int64_t pts;          // 视频帧 PTS (us)
    float   pitch;        // 俯仰角 (°)
    float   roll;         // 横滚角 (°)
    float   yaw;          // 偏航角 (°)
    int32_t batteryLevel; // 电量 0~100
};

// ============================================================
// 帧统计 (全 atomic, 任意线程可读)
// ============================================================
struct FrameStats {
    std::atomic<uint64_t> framesIn{0};        // 入队帧数
    std::atomic<uint64_t> framesDecoded{0};   // 解码输出帧数
    std::atomic<uint64_t> framesDropped{0};   // 主动丢弃(过期)帧数
    std::atomic<uint64_t> naluParseErrors{0}; // NALU 解析错误次数
};

// ============================================================
// 无锁 SPSC 环形队列 (单生产者单消费者)
// ============================================================
template <typename T, size_t CAP>
class LockFreeRingBuffer {
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of 2");
public:
    bool push(const T& v) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) & MASK;
        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) return false;
        }
        buffer_[tail] = v;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;
        }
        out = buffer_[head];
        head_.store((head + 1) & MASK, std::memory_order_release);
        return true;
    }

    void clear() {
        head_.store(0);
        tail_.store(0);
        head_cache_ = 0;
        tail_cache_ = 0;
    }

private:
    static constexpr size_t MASK = CAP - 1;
    T                       buffer_[CAP];
    std::atomic<size_t>     head_{0};
    std::atomic<size_t>     tail_{0};
    // 缓存对端索引, 减少 acquire 操作
    size_t                  head_cache_{0};
    size_t                  tail_cache_{0};
};

// ============================================================
// 实时线程辅助：绑核 + SCHED_FIFO
// Android NDK 的 sched.h 提供的是 sched_setaffinity(pid_t, size_t, cpu_set_t*)
// 不像 glibc 那样有 pthread_setaffinity_np, 这里用 gettid() 走内核接口
// ============================================================

class RealTimeThread {
public:
    static bool pinToBigCores(std::thread& t, int priority = 80) {
        cpu_set_t set;
        CPU_ZERO(&set);
        int n = sysconf(_SC_NPROCESSORS_CONF);
        // 取后 4 个核作为"大核"候选 (适用于 big.LITTLE 4+4; 纯 4 核全绑)
        for (int i = std::max(0, n - 4); i < n; ++i) {
            CPU_SET(i, &set);
        }
        // Android NDK 提供的接口: sched_setaffinity(pid, setsize, set)
        pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
        int rc = sched_setaffinity(tid, sizeof(set), &set);
        if (rc != 0) {
            LOGW("sched_setaffinity failed: %d", rc);
        }

        struct sched_param param{};
        param.sched_priority = priority;
        // pthread_setschedparam 在 Android 上可用, 但需要权限, 失败时静默降级
        int s_rc = pthread_setschedparam(t.native_handle(), SCHED_FIFO, &param);
        if (s_rc != 0) {
            LOGW("pthread_setschedparam SCHED_FIFO failed: %d (no permission, fallback to normal)", s_rc);
        }
        return rc == 0 && s_rc == 0;
    }
};

// ============================================================
// H.264 NALU Demuxer：只做定位, 不做解码语义分析
// 状态机:  搜索 0x000001 / 0x00000001 起始码
// ============================================================
class H264Demuxer {
public:
    // 描述一个完整的 NALU (含起始码)
    struct Nalu {
        const uint8_t* data;
        size_t         size;     // 包含起始码
        uint8_t        type;     // NALU 类型 (低 5 bit)
    };

    // 推入一帧网络数据, 内部累积, 切出完整的 NALU 通过回调上报
    // 单次推入的 buffer 内可能包含 1~N 个 NALU
    void feed(const uint8_t* data, size_t len,
              const std::function<void(const Nalu&)>& onNalu) {
        if (len == 0) return;
        buffer_.insert(buffer_.end(), data, data + len);

        // 在累积 buffer 中切 NALU
        scan(onNalu);
    }

    void reset() {
        buffer_.clear();
    }

private:
    std::vector<uint8_t> buffer_;

    // 寻找下一个起始码 (3 字节 0x000001 或 4 字节 0x00000001)
    // 返回起始码在 buffer 中的下标, 以及起始码长度 (3 或 4)
    static ssize_t findStartCode(const uint8_t* p, size_t len, int& scLen) {
        if (len < 3) return -1;
        for (size_t i = 0; i + 2 < len; ++i) {
            if (p[i] == 0 && p[i + 1] == 0) {
                if (p[i + 2] == 1) {
                    scLen = 3;
                    return i;
                }
                if (i + 3 < len && p[i + 2] == 0 && p[i + 3] == 1) {
                    scLen = 4;
                    return i;
                }
            }
        }
        return -1;
    }

    void scan(const std::function<void(const Nalu&)>& onNalu) {
        // 找第一个起始码, 之前的数据 (没有起始码前缀的残留) 直接丢掉
        int firstScLen = 0;
        ssize_t firstSc = findStartCode(buffer_.data(), buffer_.size(), firstScLen);
        if (firstSc < 0) {
            // 整段 buffer 都没起始码, 大概率是上一个 NALU 的尾巴还没拼完整, 等下一包
            // 但 buffer 累积太多会爆内存, 设个上限
            if (buffer_.size() > 4 * 1024 * 1024) {
                LOGW("Demuxer buffer overflow (%zu bytes) without start code, dropping", buffer_.size());
                buffer_.clear();
            }
            return;
        }

        // 从第一个起始码开始, 成对扫描: 起始码 A 和 起始码 B 之间是一个完整 NALU
        size_t prevScStart = (size_t)firstSc;
        size_t prevScLen   = (size_t)firstScLen;
        size_t cursor      = prevScStart + prevScLen;  // 跳过第一个起始码

        while (cursor < buffer_.size()) {
            int nextScLen = 0;
            ssize_t nextSc = findStartCode(buffer_.data() + cursor,
                                            buffer_.size() - cursor, nextScLen);
            if (nextSc < 0) {
                // 找不到下一个起始码 → 当前 NALU 还没收完, 不能下发
                // 保留从 prevScStart 开始的所有字节, 等下个 SRT 包拼上来
                break;
            }

            // prevScStart 到 nextSc 之间是完整 NALU (含前一个起始码, 不含后一个)
            size_t naluEnd = cursor + (size_t)nextSc;
            Nalu n;
            n.data = buffer_.data() + prevScStart;
            n.size = naluEnd - prevScStart;
            n.type = (n.data[prevScLen] & 0x1F);
            onNalu(n);

            // 推进到下一个 NALU
            prevScStart = naluEnd;
            prevScLen   = (size_t)nextScLen;
            cursor      = naluEnd + (size_t)nextScLen;
        }

        // 把已下发过的字节裁掉, 保留最后一段 (可能是不完整的下一个 NALU, 含其起始码)
        if (prevScStart > 0) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + prevScStart);
        }
    }
};

// ============================================================
// DroneMediaEngine：解码器 + 解析 + 帧统计
// ============================================================
class DroneMediaEngine {
public:
    DroneMediaEngine() = default;
    ~DroneMediaEngine() { release(); }

    // 初始化硬件解码器
    // mime:  "video/avc" (H.264) 或 "video/hevc" (H.265)
    // width/height: 视频尺寸, 用于初始化解码器
    // surface: 已经绑定 OES 纹理的 Surface (由 GL 线程创建)
    void init(JNIEnv* env, jstring jMime, jint width, jint height, jobject surface);

    // 推入一帧 H.264/H.265 原始字节流 (从 UDP 套接字 / OTG / 文件中读取到的)
    // 这是 SPSC 模型的 producer
    void feedStream(const uint8_t* data, size_t size, int64_t pts);

    // 在 GL 线程上回调, 用于根据当前 PTS 查询最新遥测数据
    // 返回 false 表示没有匹配的遥测数据
    bool queryTelemetry(int64_t ptsUs, TelemetryData& out);

    // 任意线程可写, 写入最新的遥测快照 (供查询)
    void injectTelemetry(const TelemetryData& t);

    // 启动后台线程 (网络接收/解码输出)
    void start();
    void stop();
    void release();

    FrameStats& stats() { return stats_; }

private:
    // 解码器
    AMediaCodec*     codec_      = nullptr;
    ANativeWindow*   window_     = nullptr;
    jobject          surface_ref_ = nullptr;   // 全局引用, 防止 Surface 被 GC
    std::string      mime_;
    int32_t          width_      = 0;
    int32_t          height_     = 0;
    int64_t          input_pts_  = 0;

    // 解析
    H264Demuxer      demuxer_;

    // 待送入解码器的 NALU 队列 (producer=feedStream 线程, consumer=decoder input 线程)
    struct PendingNalu {
        std::vector<uint8_t> data;
        int64_t              pts;
    };
    LockFreeRingBuffer<PendingNalu, 64> nalu_queue_;

    // 缓存最近一次 SPS/PPS, 供 IDR 到来时一起送入解码器
    std::vector<uint8_t> cached_sps_;
    std::vector<uint8_t> cached_pps_;
    bool                 codec_configured_ = false;  // SPS+PPS 是否已下发

    // 遥测
    std::mutex                             telemetry_mutex_;
    TelemetryData                          latest_telemetry_{};
    bool                                   has_latest_telemetry_ = false;

    // 后台线程
    std::thread      input_thread_;
    std::thread      output_thread_;
    std::atomic<bool> running_{false};

    FrameStats       stats_;

    // 解码器输入端: 从 nalu_queue_ 取数据塞给 codec
    void inputLoop();
    // 解码器输出端: 循环 drain output, 释放 buffer (zero-copy 模式下, release 即可)
    void outputLoop();

    // 把一个 NALU 送入解码器 input buffer
    void enqueueNalu(const uint8_t* data, size_t size, int64_t pts);

    // 处理一个完整的 NALU (来自 demuxer)
    void handleNalu(const H264Demuxer::Nalu& n);
};
