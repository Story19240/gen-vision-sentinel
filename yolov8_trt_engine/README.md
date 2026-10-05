# TensorRT 原生极速推理底层引擎 (`trt_engine`)

本工程为发电机视觉预警系统的**公共算力底座**，面向 NVIDIA Jetson AGX Xavier (16GB) 深度定制，基于纯 C++ (C++14) 与 TensorRT 8.5 原生 API 构建，提供毫秒级、高吞吐的目标检测推理能力。

---

## 目录结构

```text
trt_engine/
├── 📁 include/      # 头文件目录 (types.h, yolov8_trt.h)
├── 📁 src/          # 源码实现目录 (yolov8_trt.cpp, main.cpp)
├── 📄 Makefile      # 自动化编译脚本 (一键编译生成 yolov8_app)
└── 📑 README.md     # 本工程说明文档
```

---

## 头文件说明 (`include/`)
* **`include/types.h`**：定义检测目标结构体 `Detection`（包含类别 ID `class_id`、置信度 `confidence`、类别名 `class_name` 及检测框像素坐标 `x1, y1, x2, y2`）。
* **`include/yolov8_trt.h`**：声明 `YOLOv8TRT` 推理类接口与生命周期管理函数。

---

## 编译与测试指南 (Jetson AGX Xavier)

### 1. 一键编译
在工程根目录下直接调用 `make`：
```bash
cd /data/workspace/yolov8_trt_rtsp   # (对应板载工程目录)
make clean && make -j4
```
编译产物为原生二进制程序：**`yolov8_app`**。

### 2. 运行单图检测验证
```bash
# 执行实测验证并输出画框图片 annotated_result.jpg
./yolov8_app /data/models/yolov8n_fp16.engine /data/workspace/frame_033.jpg
```

---

## 实测性能指标 (Jetson AGX Xavier 满血 MAXN 模式)

* **单帧端到端耗时**：**11.9 ~ 12.3 ms**（包含 Letterbox 预处理、CUDA 显存搬运、FP16 卷积推理、NMS 去重全流程）
* **实时吞吐量**：**80.8 ~ 83.6 FPS**
* **工业意义**：单路摄像头标准帧率仅为 25 FPS，当前引擎具备支撑 **3~4 路 1080P 工业摄像机** 同时全并发检测的充沛算力余量。
