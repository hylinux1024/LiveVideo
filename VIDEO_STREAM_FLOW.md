# LiveVideo 视频流播放流程

## 0. 端到端架构

```
┌──────────────┐  SRT/UDP   ┌────────────────────────────┐  Surface  ┌──────────────┐
│  ffmpeg 推流  │ ─────────► │  libsrt (native thread)    │ ────────► │  AMediaCodec │
│  192.168.31.x │  1234/1236 │  srt_listen + srt_recvmsg  │  OES 纹理 │  硬件解码器   │
└──────────────┘            │           │                 │           └──────┬───────┘
                            │           ▼ JNI 回调        │                  │ GraphicBuffer
                            │  LiveVideoEngineJNI.feedStream  │                  ▼
                            │           │                 │           ┌──────────────┐
                            │           ▼                 │           │ SurfaceTexture│
                            │  H264Demuxer.scan           │           │  OES 纹理     │
                            │           │                 │           └──────┬───────┘
                            │           ▼                 │                  │ updateTexImage
                            │  nalu_queue_ (SPSC)         │                  ▼
                            │           │                 │           ┌──────────────┐
                            │           ▼                 │           │ GL 渲染线程   │
                            │  AMediaCodec_dequeueInput   │           │  glDrawArrays│
                            │  + queueInputBuffer         │           └──────┬───────┘
                            └────────────────────────────┘                  ▼
                                                                  Android 屏幕
```

---

## 1. 推流端 (ffmpeg)

```bash
ffmpeg -re -i test.mp4 \
  -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
  -g 30 -bf 0 -tune zerolatency -preset ultrafast \
  -an -f h264 \
  'srt://192.168.31.30:1234?latency=120'
```

| 参数 | 作用 |
|------|------|
| `-re` | 按真实时间戳读帧, 模拟直播 |
| `-profile:v baseline` | H.264 baseline, 兼容性最强, 手机硬解必吃 |
| `-level 3.1` | 限定码率/分辨率在 720p@30 以内 |
| `-g 30` | 每 30 帧一个 IDR (1 秒 1 个关键帧) |
| `-bf 0` | 禁 B 帧, 降延迟 |
| `-tune zerolatency` | x264 调优, 关掉 lookahead/RCU lookahead |
| `-preset ultrafast` | 最快编码, 质量换速度 |
| `-an` | 不要音频 |
| `-f h264` | 输出 H.264 Annex-B 字节流 (非 mpegts) |
| `latency=120` | SRT 内部缓冲 120ms |

**fmp4 → Annex-B**: 推 `-f h264` 时 ffmpeg 内部加 `h264_mp4toannexb` bsf, 在每个 NALU 前插 `00 00 00 01` 起始码。

---

## 2. SRT 接收端 (native)

### 2.1 启动流程

`MainActivity.onCreate` → `SrtBridge` 触发 `System.loadLibrary("livevideo")` → `JNI_OnLoad` → `initSrtSubsystem(vm)` 调 `srt_startup()` → `nativeInitBindings()` 缓存 class/methodID。

`SrtBridge.nativeStart(1234)` → `SrtListenerImpl::start()` → 新建 `std::thread` 跑 `runLoop()`。

### 2.2 关键 SRT 参数 (`SrtListener.cpp:90-115`)

| 选项 | 值 | 作用 |
|------|----|----|
| `SRTO_REUSEADDR` | 1 | 端口 TIME_WAIT 后能立刻 rebind |
| `SRTO_LATENCY` / `SRTO_RCVLATENCY` | 120ms | 收发端延迟容忍 (抗 30% 丢包) |
| `SRTO_TSBPDMODE` | 0 | **关闭** TSBPD 时间戳缓冲, 不要这个会引入 120ms 固定延迟 |
| `SRTO_PAYLOADSIZE` | 1316 | 单包负载, ≤ MTU 1500 减 UDP/IP/SRT 头 |
| `SRTO_RCVBUF` / `SRTO_UDP_RCVBUF` | 12MB | 大缓冲抗突发 |
| `SRTO_LOSSMAXTTL` | 30 | 30 跳未到的包直接丢, 不无限重传 |
| `SRTO_OHEADBW` | 25% | 25% 带宽预留给重传/FEC |
| `SRTO_MAXBW` | 0 | 不限速 |

### 2.3 收发循环 (`runLoop`)

```
srt_create_socket
  ↓
srt_setsockopt × N  (上面那些参数)
  ↓
srt_bind 0.0.0.0:1234
  ↓
srt_listen(backlog=5)
  ↓
loop:
  srt_accept (阻塞, 无客户端时停在这里)
  handleClient:
    loop:
      srt_recvmsg(buf, 64KB)
      ↓
      dispatchPacket → JNI CallStaticVoidMethod(LiveVideoEngineJNI.feedStream)
```

`dispatchPacket` 在 native 线程上, JNIEnv 通过 `GetEnv`/`AttachCurrentThread` 拿。

---

## 3. JNI 桥接 (`SrtListener.cpp → LiveVideoEngineJNI`)

```cpp
// SrtListener.cpp
env->CallStaticVoidMethod(g_engine_class, g_feedStream_mid, jbyteArray, pts=0);
```

```kotlin
// LiveVideoEngineJNI.kt
@JvmStatic
external fun feedStream(data: ByteArray, pts: Long = 0L)
```

```cpp
// LiveVideoEngine.cpp
Java_com_livevideo_media_LiveVideoEngineJNI_feedStream(JNIEnv* env, jclass, jbyteArray, jlong) {
    g_engine->feedStream(buf, len, pts);
}
```

**关键**: 必须 `@JvmStatic` + C++ 第二个参数 `jclass`, 否则 `GetStaticMethodID` 拿不到 (Kotlin `object` 默认生成实例方法, SrtListener 错用 `GetStaticMethodID` 会返回 null, 静默失败导致黑屏)。

**类加载器坑**: SRT 是 native 线程, bootstrap classloader 找不到 app 类 (`ClassNotFoundException`)。修法: 第一次 `FindClass`/`GetStaticMethodID` 在主线程做 (`SrtBridge.init → nativeInitBindings()`), 缓存全局引用, 后续 SRT 线程直接用缓存。

---

## 4. H.264 Demuxer (`LiveVideoEngine.h:H264Demuxer`)

把不规整的字节流切成完整 NALU。

### 4.1 状态机

```
feed(bytes) → buffer_.insert(bytes)
           → scan()
              → findStartCode(00 00 01 / 00 00 00 01) 找下个 NALU 起点
              → 上一个起点到这个起点之间是一个完整 NALU, onNalu 回调
              → 没找到下一个起点 → 整个 NALU 不完整, 留在 buffer 等下一包
              → 把已处理字节 erase
```

### 4.2 关键 bug 修法

之前花屏: ffmpeg 把 NALU 切成 1316 字节的 SRT 块, 长 NALU 跨多个 SRT 包。demuxer 旧逻辑:

```cpp
if (nextSc < 0) after = buffer_.size();   // ❌ 把残余当完整 NALU 上报
onNalu({data: naluStart, size: buffer_.size() - naluStart});
```

修法: `nextSc < 0` 时 `break`, 不上报, 保留 buffer 等下个 SRT 包拼上, 重新从已保留的起始码开始扫描。

### 4.3 NALU 分类 (`LiveVideoEngine.cpp:handleNalu`)

| nalType | 含义 | 处理 |
|---------|------|------|
| 7 (SPS) | 序列参数集 | 缓存到 `cached_sps_` |
| 8 (PPS) | 图像参数集 | 缓存到 `cached_pps_` |
| 5 (IDR) | 关键帧 | 先下发 SPS+PPS (`codec_configured_=true`), 再下发本帧 |
| 1 (P) | 普通帧 | 计数后下发 |
| 6 (SEI) | 补充信息 | 丢弃 |

**坑**: SPS/PPS 不在第一个 SRT 包, demuxer 必须先扫到 7 再扫到 8 再等到第一个 IDR 才能下发 codec config。

---

## 5. 硬件解码 (`AMediaCodec`)

### 5.1 初始化 (`LiveVideoEngine::init`)

```cpp
AMediaCodec_createDecoderByType("video/avc")
  ↓
AMediaFormat_setInt32  WIDTH=320, HEIGHT=240
AMediaFormat_setInt32  "low-latency" = 1
AMediaFormat_setInt32  "vendor.qti-ext-dec-low-latency.enable" = 1  // 高通
AMediaFormat_setInt32  "vendor.mtk.ext-dec-low-latency.enable" = 1  // 联发科
  ↓
AMediaCodec_configure(codec, format, ANativeWindow, NULL, 0)  // 零拷贝: surface 模式
  ↓
AMediaCodec_start
```

**零拷贝关键**: `configure` 时传入 ANativeWindow, 解码器把 YUV 直接写到 GraphicBuffer, 经 SurfaceTexture 投到 OES 纹理, 不进 CPU 内存。

**尺寸 320x240 是 hint**: 真实分辨率以码流 SPS 为准, 解码器通过 `Output format changed` 回调报出实际尺寸。

### 5.2 输入线程 (`inputLoop`)

```
loop:
  nalu_queue_.pop()                    // SPSC 队列, 阻塞 200us
  ↓
  AMediaCodec_dequeueInputBuffer(2ms)
    TRY_AGAIN_LATER → push 回队列, sleep 500us
    有效 idx
  ↓
  AMediaCodec_getInputBuffer(idx, &bufCap)
  memcpy(buf, nalu.data, nalu.size)
  ↓
  AMediaCodec_queueInputBuffer(idx, offset, size, pts, 0)
    isFrame (nalType 1/5) → pts = input_pts_ += 33333  (30fps)
    SPS/PPS/SEI            → pts = 0
```

### 5.3 输出线程 (`outputLoop`)

```
loop:
  AMediaCodec_dequeueOutputBuffer(&info, 1s)
    TRY_AGAIN_LATER → continue
    OUTPUT_FORMAT_CHANGED → log 真实分辨率
    有效 idx
  ↓
  AMediaCodec_releaseOutputBuffer(idx, true)  // render=true: 驱动自动投到 ANativeWindow
  stats.framesDecoded++
```

`render=true` 是零拷贝核心: 驱动内部把 GraphicBuffer 的内容直接贴到 SurfaceTexture, 不需要 `getOutputBuffer` 拷到 CPU。

---

## 6. 渲染 (`LiveVideoView`)

### 6.1 初始化

```kotlin
GLES20.glGenTextures → oesTextureId
GLES20.glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTextureId)
SurfaceTexture(oesTextureId)   // 把纹理 ID 交给 SurfaceTexture
Surface(surfaceTexture)        // 包装成 Surface 传给 C++
LiveVideoEngineJNI.initDecoder("video/avc", 320, 240, surface)
```

### 6.2 着色器

```glsl
// Vertex
attribute vec2 aPosition, aTexCoord;
varying vec2 vTexCoord;
gl_Position = vec4(aPosition, 0, 1);
vTexCoord = aTexCoord;

// Fragment
#extension GL_OES_EGL_image_external : require
uniform samplerExternalOES uTexture;
gl_FragColor = texture2D(uTexture, vTexCoord);
```

### 6.3 顶点/UV

```kotlin
// 屏幕 NDC: -1,-1 在左下, 1,1 在右上
val verts = floatArrayOf(
    -1f, -1f,  // 左下
     1f, -1f,  // 右下
    -1f,  1f,  // 左上
     1f,  1f   // 右上
)
// OES UV: (s=0, t=0) 是图像**左上**, 与屏幕 NDC Y 轴反向
val texs = floatArrayOf(
    0f, 1f,    // 屏幕左下 -> 图像左下
    1f, 1f,    // 屏幕右下 -> 图像右下
    0f, 0f,    // 屏幕左上 -> 图像左上
    1f, 0f     // 屏幕右上 -> 图像右上
)
```

**坑**: OES 纹理用图像坐标系 (左上原点), 不翻 Y 视频会倒立。

### 6.4 绘制循环 (`onDrawFrame`)

```kotlin
synchronized(frameLock) {
    if (pendingFrame) {
        surfaceTexture.updateTexImage()  // 拉取最新 GraphicBuffer 到 OES 纹理
        pendingFrame = false
    }
}
glUseProgram(program)
glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTextureId)
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)    // 画全屏四边形
```

`OnFrameAvailableListener` 由驱动在 `releaseOutputBuffer(render=true)` 后异步回调, 置 `pendingFrame=true` + `requestRender()`。

---

## 7. 线程模型

| 线程 | 创建处 | 职责 |
|------|--------|------|
| Main (UI) | Android | HUD 更新, 模式切换, 统计刷新 |
| GL Render | GLSurfaceView | `updateTexImage` + `glDrawArrays` |
| SRT Native | `SrtListener::runLoop` | `srt_listen`/`srt_recvmsg`, JNI 回调 `feedStream` |
| Input | `LiveVideoEngine::start` | `nalu_queue.pop` → `AMediaCodec_dequeueInputBuffer` → `queueInputBuffer` |
| Output | `LiveVideoEngine::start` | `AMediaCodec_dequeueOutputBuffer` → `releaseOutputBuffer(true)` |

SPSC 队列 `LockFreeRingBuffer<Nalu, 64>` 解耦网络接收和解码输入, `relaxed/acquire/release` 内存序避免 false sharing。

`sched_setaffinity` 绑大核 (4~7 核) + `SCHED_FIFO 70~80` 给 input/output 线程, 降低解码抖动。

---

## 8. 统计 / 调试

```kotlin
val stats = LiveVideoEngineJNI.getStats()  // [IN, DEC, DROP, parseErr]
```

| 字段 | 含义 | 异常诊断 |
|------|------|----------|
| `framesIn` | `feedStream` 调用次数 | 0 → SRT 收不到包 |
| `framesDecoded` | `releaseOutputBuffer` 成功 | 不增长 → 缺 SPS/PPS 或解码器配置错 |
| `framesDropped` | 队列满/未配置丢帧 | 高 → 解码太慢或网络太快 |
| `naluParseErrors` | demuxer 找不到起始码 | > 0 → 数据格式错 (如推了 mpegts) |

### 8.1 FPS 计算

`MainActivity.statsUpdater` 主线程每 200ms 拉一次 stats, 用 `DEC` 差值除以时间差:

```kotlin
val nowNs = System.nanoTime()
val fps = (dec - lastDec) * 1_000_000_000.0 / (nowNs - lastUpdateNs)
```

| 现象 | 含义 |
|------|------|
| FPS ≈ 30 (推流 30fps 时) | 端到端正常, 推流帧全部进解码器 |
| FPS < 30 | decoder 跟不上, 优先看 DROP 是不是 > 0, 排查解码器性能或降分辨率 |
| FPS = 0 但 DEC 持续涨 | 时间窗太短或 DEC 增量过小, 拉到 1 秒窗更准 |
| FPS 显示毛刺 (一会儿 50 一会儿 0) | `releaseOutputBuffer` 不均匀, 正常, 看 1 分钟平均更准 |

---

## 9. 关键路径耗时分解 (推流→屏幕)

| 阶段 | 耗时 |
|------|------|
| ffmpeg 编码 1 帧 | 5-15ms (ultrafast) |
| SRT 链路 (同 WiFi) | 5-30ms (含 ARQ 重传) |
| 收包 → demuxer 切 NALU | < 1ms |
| NALU 入队 → input 线程取 | < 1ms |
| `queueInputBuffer` → 解码 | 5-15ms (硬件) |
| `releaseOutputBuffer` → GPU 投屏 | 1 帧 (33ms@30fps) |
| **端到端** | **50-90ms** |

SRT latency=120ms + 解码 1 帧 = 理论下限 ~150ms。实测 30fps 下约 80-120ms (因为提前一帧解码在 pipeline 里)。

---

## 10. NALU 是什么

**NALU = Network Abstraction Layer Unit**, H.264/AVC 码流的基本传输单位。

### 10.1 H.264 的两层结构

| 层 | 职责 |
|----|------|
| **VCL** (Video Coding Layer) | 真正做压缩: 帧内预测、帧间预测、DCT、量化、熵编码 |
| **NAL** (Network Abstraction Layer) | 把 VCL 产出的数据打包成 NALU, 加上时间/类型/边界信息, 方便传输 |

NAL 把 VCL 的裸 bit 流切成独立单元, 每个 NALU 自带类型信息, 让解码器知道这一包是啥、要不要解、跟前一帧啥关系。

### 10.2 NALU 字节布局

```
┌────────────┬────────────┬─────────────────┐
│  起始码     │  1 字节头   │   RBSP 负载     │
│ 00 00 00 01│             │   (实际压缩数据) │
│  或         │             │                 │
│ 00 00 01   │             │                 │
└────────────┴────────────┴─────────────────┘
```

**1 字节头** 拆 3 段:

```
 bit:  7      6 5       4 3 2 1 0
       │      │         │
       └─┐  ┌─┴──┐   ┌──┴────┐
         │  │    │   │       │
       F  NRI   nal_unit_type
       │   │         │
       │   │         └─ 类型 (0~31)
       │   └─ 重要性 (0~3, 越大越不能丢)
       └─ 禁止位 (恒为 0, 出错检测用)
```

### 10.3 主要 nal_unit_type (demuxer 在识别这些)

| type | 名称 | 含义 | 在代码里的处理 |
|------|------|------|-------------------|
| **1** | non-IDR slice | 普通 P 帧 (参考前一帧) | `enqueueNalu`, pts 递增 33333us (30fps) |
| **5** | IDR slice | **关键帧**, 强制清空参考缓冲, 解码器从这里能独立开始 | 先下发缓存的 SPS+PPS (`codec_configured_=true`), 再下发本帧 |
| **6** | SEI | 补充信息 (时间戳、字幕、镜头信息...) | **丢弃** (用不上) |
| **7** | SPS | 序列参数集 (分辨率/帧率/profile/参考帧数...) | 缓存到 `cached_sps_`, 等 IDR 一起发 |
| **8** | PPS | 图像参数集 (熵编码方式/去块滤波...) | 缓存到 `cached_pps_`, 等 IDR 一起发 |
| **9** | AUD | 访问单元分隔符 (一帧的边界) | 不出现, 可忽略 |

### 10.4 一段真实码流

ffmpeg 推的 320x240 baseline 30fps 流的某个时间片, demuxer 切出来大致长这样:

```
00 00 00 01 67 ...    ← SPS (type=7),   ~20 字节
00 00 00 01 68 ...    ← PPS (type=8),   ~5 字节
00 00 00 01 65 ...    ← IDR (type=5),   ~3000 字节
00 00 00 01 41 ...    ← P (type=1),     ~500 字节
00 00 00 01 41 ...    ← P
... (29 个 P 帧)
00 00 00 01 65 ...    ← 下一个 IDR (g=30, 每 30 帧一个)
00 00 00 01 41 ...
...
```

**关键字节解析**:
- **`67`** = `0110 0111` = F:0 NRI:3 type:7 → SPS, 不许丢
- **`68`** = `0110 1000` = F:0 NRI:3 type:8 → PPS, 不许丢
- **`65`** = `0110 0101` = F:0 NRI:3 type:5 → IDR, 关键帧
- **`41`** = `0100 0001` = F:0 NRI:2 type:1 → 普通 P 帧, 丢了花屏但能继续解下一个 IDR

### 10.5 为什么 demuxer 必须能切 NALU

**裸 H.264 Annex-B 字节流** 只是一长串 NALU 拼起来, 没有 frame 头、没有总长度, 唯一的边界就是 `00 00 00 01` 起始码。所以 demuxer 的工作就是:
1. 找起始码
2. 切出完整的 NALU
3. 读第一个字节判断 type, 分发到 SPS/PPS/IDR/P/SEI 不同的处理路径

### 10.6 对应代码

`LiveVideoEngine.cpp:handleNalu()`:
```cpp
uint8_t nalType = n.data[scLen] & 0x1F;  // 取低 5 位就是 type
if (nalType == 7) cached_sps_ = ...;     // 缓存 SPS
if (nalType == 8) cached_pps_ = ...;     // 缓存 PPS
if (nalType == 5) {                      // IDR 来了
    下发 cached_sps_ + cached_pps_;      // 解码器配置
    codec_configured_ = true;
    下发本帧;
}
if (nalType == 1) 下发 P 帧;             // 普通帧
if (nalType == 6) 跳过;                  // SEI 不用
```

`LiveVideoEngine.cpp:inputLoop()`:
```cpp
// PTS 只给 IDR/P, 不给 SPS/PPS (它们是 codec config, 不算"帧")
bool isFrame = (nalType == 1 || nalType == 5);
int64_t pts = isFrame ? (input_pts_ += 33333) : 0;
```
