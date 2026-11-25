# 火花检测 C++ 离线版

本目录包含 `spark/prototype/spark_detector.py` 的 C++ 离线移植和独立在线监控服务。检测核心不依赖 OpenCV；完整媒体读取和标注图输出依赖 OpenCV 4.x 兼容 API。离线工具不接 RTSP、飞书或继电器；在线服务见下节。

## 在线监控与 Web 标定服务

`spark_monitor_server` 复用同一套检测核心，提供 RTSP H.265 硬解、8081 Web 实时监控、三类 ROI 鼠标标定、配置持久化、参数热更新、事件抓拍与前后片段录像，以及飞书“疑似火花”异步卡片。确认异常后可给继电器 Y1 通电吸合；该联动默认禁用，须在网页明确启用。

在 Jetson 仓库根目录启动：

```bash
bash spark/src/run_spark.sh
```

浏览器打开 `http://192.168.1.100:8081`（USB 调试通道为 `http://192.168.55.1:8081`）。生产配置写入 `spark/spark_config.json`，事件产物写入 `spark/recordings/`。启动后默认处于标定模式，三类 ROI 均有效后才能进入监控。标定绑定当前分辨率，分辨率变化会要求重新标定。

RTSP 默认读取 `SPARK_RTSP_URL`、`RTSP_URL` 或 Jetson 的 `/root/.rtsp_url`，不会打印完整地址。飞书读取 `spark_config.json` 的 `feishu` 配置块，缺失时回退到 `.env` 中的同名变量。可用网页设置卡片跳转地址。

### 继电器 Y1 联动

在网页“继电器联动”中勾选“确认异常时 Y1 通电”并保存后，检测器确认 `event_triggered` 时才向 Y1 下发电吸合指令；候选帧不会触发。动作采用告警锁存，避免图像抖动造成反复通断。操作员可在网页点击“Y1 断电复位”；进入标定、视频源结束或服务正常退出也会下发电指令。

继电器地址优先读取配置的 `relay.host` / `relay.port`，未填写时回退到 `RELAY_HOST` / `RELAY_PORT`，默认 `192.168.0.7:8234`。`relay_trip_enabled` 默认为 `false`，防止离线视频联调意外触发现场设备。火花进程只自动复位自己管理的 Y1，不复位水流项目使用的 Y2～Y8。页面“通电指令”表示指令已交给异步客户端，不等同于已读取触点状态；现场投运前仍需核对接线并进行真实动作验收。

无硬件回归测试使用回环假继电器，不连接现场设备：

```bash
cd spark/src
make test-monitor-relay
python3 test_relay_server.py 18234 /tmp/spark-relay-packets.log &
bin/test_monitor_relay ../spark_config.example.json /tmp/spark-relay-test 18234 20261105
wait
```

通过标记为 `MONITOR_RELAY_TEST_OK` 与 `RELAY_PACKETS_OK`；假继电器还会拒绝任何 Y2～Y8 报文。

离线视频联调示例：

```bash
spark/src/bin/spark_monitor_server \
  --input spark/videos/mmexport1793887748039.mp4 \
  --config spark/prototype/local_ai720_config.json \
  --recordings /tmp/spark-monitor-test \
  --port 8081 --once --no-delay
```

`--once` 播放结束后保留 Web 服务，便于检查事件与录像；`--no-delay` 仅用于测试，生产实时流不需要。

## 源码组成

| 文件 | 职责 |
| --- | --- |
| `json.hpp` / `json.cpp` | 严格 JSON 解析、加载与输出 |
| `spark_detector.h` / `spark_detector.cpp` | ROI 解析、OpenCV 等价 BGR 转 HSV、四路信号、200ms 状态机 |
| `offline_io.hpp` / `offline_io.cpp` | `metrics.csv`、`summary.json`、基线和标定配置输出 |
| `synthesize.hpp` / `synthesize.cpp` | 七个确定性合成场景；随机流使用 C++，不保证与 NumPy 逐字节一致 |
| `opencv_source.hpp` / `opencv_source.cpp` | 图片/视频读取与 OpenCV 标注图输出 |
| `main.cpp` | 命令行入口、配置预检和结果落盘 |
| `compare_offline.py` | 对比两个离线结果目录的逐帧指标和事件列表 |

## 构建

在项目主本地电脑或 Jetson 上构建完整媒体版：

```bash
cd spark/src
pkg-config --modversion opencv4
make clean
make OPENCV_PKG=opencv4
```

如果 pkg-config 里的包名不是 `opencv4`，把 `OPENCV_PKG` 换成实际名称。没有 pkg-config 文件时可显式传：

```bash
make \
  OPENCV_CFLAGS="-I/usr/include/opencv4" \
  OPENCV_LIBS="-lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio"
```

云端工作站没有 OpenCV，只能构建无媒体版冒烟目标：

```bash
cd spark/src
make no-opencv
./bin/spark_offline_no_opencv --scenario blue_white_spark \
  --config ../spark_config.example.json \
  --output /tmp/spark_cpp_blue_white \
  --fps 25 --frames 60 --seed 20261105
```

无媒体版不支持 `--input`，也不输出 `annotated.png`；它用于验证检测核心、JSON、CSV/JSON 输出和状态机。

## 运行口径

```bash
# 图片
./bin/spark_offline \
  --input ../images/carbon_brush_sample.png \
  --config ../spark_config.example.json \
  --output /tmp/spark_cpp_sample

# 视频
./bin/spark_offline \
  --input /path/to/video.mp4 \
  --config /path/to/calibrated_config.json \
  --output /tmp/spark_cpp_video
```

参数与 Python 原型保持一致：`--input`、`--scenario`、`--config`、`--output`、`--baseline`、`--fps`、`--frames`、`--seed`。输出为 `metrics.csv`、`summary.json`；合成场景和图片输入另写 `annotated.png`；加 `--baseline` 时另写 `baseline.json` 和 `calibrated_config.json`。

算法保持以下语义：四路物理信号同时作为三道门；0.5 秒窗口帧差；三道门连续满足 `confirm_duration_ms`（默认 200ms）后只上报一次；候选中断后重新布防；三类 ROI 缺失或非法时在写结果前报错并退出 2。相机曝光/增益仍需现场核对，正式 RTSP 与真实继电器动作按项目主确认后验收。

注意：合成场景使用 C++ 随机数，不追求和 Python/NumPy 的像素级一致。合成对拍只比较行为预期；逐帧数值对拍必须用同一个图片或视频素材。
