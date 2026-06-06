# LiveVideo

低延迟 Android 视频流接收 App (学习项目), native 层 SRT 接收 + 硬件解码 + 零拷贝 OES 纹理渲染。

> 本项目在 GitHub 上名为 **LiveVideo** ([hylinux1024/LiveVideo](https://github.com/hylinux1024/LiveVideo)), 仓库根目录也叫 `LiveVideo`, C++ 类 `LiveVideoEngine`。包名已统一为 `com.livevideo.media`。

- 推流: ffmpeg / OBS (SRT / UDP)
- 接收: C++ native 线程 + libsrt
- 解码: AMediaCodec (硬解) + Surface 直接投到 OES 纹理
- 渲染: GLSurfaceView, 零拷贝
- 端到端延迟: 同 WiFi 实测 80-120ms

| 文档 | 内容 |
|------|------|
| [VIDEO_BASICS.md](./VIDEO_BASICS.md) | 视频流基础 (YUV / H.264 / NALU / 压缩原理) |
| [VIDEO_STREAM_FLOW.md](./VIDEO_STREAM_FLOW.md) | 本项目详细架构 (demuxer / 线程模型 / 零拷贝路径) |

---

## 1. 环境要求

| 工具 | 版本 |
|------|------|
| macOS | 14+ (Apple Silicon / Intel 都行) |
| Android Studio | Ladybug (2024.2) 或更新 |
| Android NDK | 27.0.12077973 |
| Android SDK | API 26+ (minSdk), 36 (target) |
| CMake | 3.22.1 (SDK 自带) |
| JDK | 17 (Gradle 8.11 兼容) |
| ffmpeg | 含 `--enable-libsrt`, 推荐 `brew install homebrew-ffmpeg/ffmpeg/ffmpeg --with-srt` |

确认 ffmpeg 支持 SRT:

```bash
ffmpeg -protocols 2>&1 | grep -E "^srt$"
# 输出: srt
```

---

## 2. 编译

```bash
cd LiveVideo          # 本地工程目录
./gradlew :app:assembleDebug
```

产物: `app/build/outputs/apk/debug/app-debug.apk`

支持的 ABI: `arm64-v8a` + `armeabi-v7a` (libsrt 静态链接, 每个 ABI 单独编译, 体积约 14MB / 12MB)。

---

## 3. 安装到手机

手机开 USB 调试, 接 Mac:

```bash
adb devices                          # 确认设备
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.livevideo.media/.MainActivity
```

查手机 IP (跟 Mac 同 WiFi):

```bash
PHONE_IP=$(adb shell ip addr show wlan0 | grep 'inet ' | awk '{print $2}' | cut -d/ -f1)
echo $PHONE_IP   # 例如 192.168.31.30
```

---

## 4. 准备测试视频

解码器初始化按 320x240 baseline 给的 hint, 推流端最好用同样尺寸:

```bash
ffmpeg -i test.mp4 -c:v libx264 -profile:v baseline -level 3.1 \
  -pix_fmt yuv420p -s 320x240 -r 30 -g 30 -bf 0 \
  -tune zerolatency -preset ultrafast -an test_320x240_baseline.mp4
```

> `test.mp4` 是项目自带的 12 分钟 1280x720 demo。

---

## 5. 推流

### SRT (生产推荐, 抗丢包, 延迟可调)

```bash
ffmpeg -re -i test_320x240_baseline.mp4 \
  -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
  -g 30 -bf 0 -tune zerolatency -preset ultrafast \
  -an -f h264 \
  'srt://'$PHONE_IP':1234?latency=120'
```

App 默认 SRT 模式, 端口 1234, 启动后自动 listen。

### UDP (调试用, 易花屏)

```bash
ffmpeg -re -i test_320x240_baseline.mp4 \
  -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
  -g 30 -bf 0 -tune zerolatency -preset ultrafast \
  -an -f h264 \
  'udp://'$PHONE_IP':1236'
```

App 内点底部"Mode: SRT"按钮切到 UDP 模式。

---

## 6. App 模式循环

启动默认 SRT, 右上角 `Mode: XXX` 按钮循环切换:

```
SRT  →  UDP  →  SRT  →  ...
```

---

## 7. 调试

### 7.1 看 logcat

```bash
adb logcat -c
adb logcat -v color LiveVideoEngine:* SrtListener:* AndroidRuntime:E *:S
```

正常推流时的关键日志:

```
SrtListener: SRT listening on 0.0.0.0:1234 (latency=120ms)
SrtListener: Client connected: 192.168.31.x:xxxxx
SrtListener: pkt #1: 1316 bytes, head=[00 00 00 01 67 ...]   ← SPS
SrtListener: pkt #2: 1316 bytes, head=[00 00 00 01 41 ...]   ← P 帧
LiveVideoEngine: Output format changed: 320x240             ← 解码器认了 SPS
```

### 7.2 右上角统计角标

```
FPS  29.8
IN   1820
DEC  1792
DROP 0
```

| 字段 | 含义 | 异常诊断 |
|------|------|----------|
| `FPS` | 滑动窗口算的解码帧率 | < 30 + DROP > 0 → 解码器跟不上 |
| `IN` | `feedStream` 调用次数 | 不增长 → SRT 没收到包 |
| `DEC` | `releaseOutputBuffer` 成功 | 不增长 → 缺 SPS/PPS, 推流端第一条 NALU 应是 SPS |
| `DROP` | 队列满 / 未配置丢帧 | > 0 → 解码器太慢或 IDR 之前丢了 SPS/PPS |

### 7.3 常见问题

| 现象 | 原因 | 修法 |
|------|------|------|
| 黑屏, logcat 无 `Output format changed` | ffmpeg 推了 `-f mpegts` 不是 `-f h264` | 改 `-f h264` (Annex-B 字节流) |
| 黑屏, logcat 有 SRT 连接但无 NALU | JNI 回调没成 (老 bug) | 用本仓库最新代码, 主线程上 `nativeInitBindings` 缓存 methodID |
| 视频倒立 | OES UV 坐标系搞反 | 用本仓库最新代码, tex Y 已翻 |
| 视频花屏 | demuxer 旧逻辑截断了 NALU | 用本仓库最新代码, `H264Demuxer::scan` 找不见下个起始码就 break |
| 杀掉 app 后 ffmpeg 报 `Input/output error` | ffmpeg 不自动重连 SRT | ffmpeg 包 `while true; do ffmpeg ...; sleep 2; done` |
| `Failed to exec spawn helper: ninja` | AS 升级到 JDK 21 改了 fork 行为 | `gradle.properties` 加 `-Djdk.lang.Process.launchMechanism=posix_spawn` |
| Android Studio 标红 `until` / `@JvmOverloads` 但能跑 | IDE 索引缓存失效 | `File → Invalidate Caches → Restart` |

---

## 8. 项目结构

```
app/
├── src/main/
│   ├── cpp/
│   │   ├── LiveVideoEngine.h / .cpp    # 解码器 + demuxer + 线程模型
│   │   ├── SrtListener.cpp              # SRT 接收 + JNI 回调 feedStream
│   │   ├── srt/                         # libsrt 1.5.4 源码 (子项目编译)
│   │   └── CMakeLists.txt
│   ├── java/com/livevideo/media/
│   │   ├── MainActivity.kt              # 模式切换 + HUD 统计
│   │   ├── LiveVideoView.kt            # GLSurfaceView + OES shader
│   │   ├── SrtBridge.kt                 # SRT JNI 桥接
│   │   ├── LiveVideoEngineJNI.kt            # 解码器 JNI 桥接
│   │   ├── TelemetryData.kt             # 遥测数据 (未接真数据)
│   │   └── UdpVideoSource.kt            # UDP 接收 (调试用)
│   └── res/                             # layout / string / theme
├── build.gradle.kts                     # NDK 27, ABI 限定
gradle/libs.versions.toml                # 版本目录
VIDEO_STREAM_FLOW.md                     # 详细架构文档
```

---

## 9. 关键调优点

| 参数 | 文件:行 | 默认 | 调优方向 |
|------|---------|------|----------|
| SRT 延迟 | `SrtListener.cpp:97` | 120ms | 抗 30% 丢包的下限, 同 WiFi 可降到 60-80ms |
| 解码器尺寸 hint | `MainActivity.kt:71` | 320x240 | 跟推流端实际尺寸对齐 |
| 帧率假设 | `LiveVideoEngine.cpp:248` | 30fps | 改 `input_pts_ += 33333` 为目标间隔 (微秒) |
| 大核亲和 | `LiveVideoEngine.cpp:115` | 后 4 核 | `big.LITTLE 4+4` 适用, 8 核全绑需改 `n - 4` → 0 |
| SCHED_FIFO 优先级 | `LiveVideoEngine.cpp:118-119` | 70/80 | 需要 root 或 system 权限, 否则静默降级 |
