# 发电机碳刷火花监测模块 (Spark Monitoring Module)

本目录归档发电机碳刷滑环打火监测、电弧捕捉算法、训练素材及模型验证资产。

---

## 目录分层结构

```text
spark/
├── 📁 images/      # 碳刷与滑环实拍基准图、Jetson 推理验证快照
├── 📁 videos/      # （预留）现场碳刷打火与正常运行实拍视频
├── 📁 src/         # （预留）火花检测专属源码与算法
└── 📑 README.md    # 本模块说明与接口规划
```

---

## 资产说明

1. **`images/carbon_brush_sample.png`**：客户提供的现场发电机碳刷、滑环运行高清实景图。
2. **`images/annotated_result.jpg`**：Jetson Xavier 原生 TensorRT FP16 推理验证图（耗时 11.9ms，83.6 FPS）。
3. **`images/latest_camera_capture.jpg`**：工业摄像机现场实机抓拍验证帧。
