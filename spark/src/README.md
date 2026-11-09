# 火花检测 C++ 离线版

本目录是 `spark/prototype/spark_detector.py` 的 C++ 离线移植。检测核心不依赖 OpenCV；完整媒体读取和标注图输出依赖 OpenCV 4.x 兼容 API。当前实现只覆盖离线文件/合成输入，不接 RTSP，不接飞书，不控制继电器。

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

算法保持以下语义：四路物理信号同时作为三道门；0.5 秒窗口帧差；三道门连续满足 `confirm_duration_ms`（默认 200ms）后只上报一次；候选中断后重新布防；三类 ROI 缺失或非法时在写结果前报错并退出 2。相机曝光/增益、RTSP 接入、独立进程部署和 Y1 动作仍属后续工作，按项目主确认后推进。

注意：合成场景使用 C++ 随机数，不追求和 Python/NumPy 的像素级一致。合成对拍只比较行为预期；逐帧数值对拍必须用同一个图片或视频素材。
