# 发电机碳刷火花监测模块 (Spark Monitoring Module)

本目录归档发电机碳刷滑环打火监测、电弧捕捉算法、训练素材及模型验证资产。
> ⚠️ **已废弃（2026-11-04 项目主定案）**：基于 YOLO 监督学习的火花检测路线（含上文"训练素材及模型验证资产"、TensorRT 推理验证图）已废弃——现场无真实火花正样本。新路线见 [碳刷火花监测模块设计文档](../docs/碳刷火花监测模块设计文档.md)。

---

## 目录分层结构

```text
spark/
├── 📁 images/      # 碳刷与滑环实拍基准图、Jetson 推理验证快照
├── 📁 videos/      # （预留）现场碳刷打火与正常运行实拍视频
├── 📁 prototype/   # 物理信号检测离线算法原型与合成验证
├── 📁 src/         # （预留）生产 C++ 火花检测源码与算法
└── 📑 README.md    # 本模块说明与接口规划
```

---

## 资产说明

1. **`images/carbon_brush_sample.png`**：客户提供的现场发电机碳刷、滑环运行高清实景图。
2. **`images/annotated_result.jpg`**：Jetson Xavier 原生 TensorRT FP16 推理验证图（耗时 11.9ms，83.6 FPS）。
3. **`images/latest_camera_capture.jpg`**：工业摄像机现场实机抓拍验证帧。

---

## 离线算法原型

物理信号路线的 Python 离线原型位于 [`prototype/`](prototype/)，用于在生产服务开发前验证四路信号判决链和合成场景，不接相机、RTSP、飞书或继电器。

- [原型说明与 conda 运行命令](prototype/README.md)
- [本地电脑待验证命令与验收标准](prototype/待验证清单.md)
- [检测器实现](prototype/spark_detector.py)
- [确定性合成注入](prototype/synthesize.py)
- [七场景自验证](prototype/self_test.py)
- [离线运行 CLI](prototype/run_offline.py)

示例配置见 [`spark_config.example.json`](spark_config.example.json)。其中 ROI 为待标定占位值，曝光与增益待现场锁定，`confirm_duration_ms=200` 为待项目主确认的原型默认值。生产 C++ 移植、相机源、进程形态及 Y1 动作方式均不在本原型范围内。
