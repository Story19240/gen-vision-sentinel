# TensorRT 推理引擎核心实现源码 (`trt_engine/src`)

本目录存放基于 NVIDIA 原生 C++ API 实现的 YOLOv8 TensorRT FP16 极速推理类与主测试程序的源码。

---

## 源码文件与架构设计

| 源码文件 | 角色定位 | 内部技术实现与核心职责 |
| :--- | :--- | :--- |
| **`yolov8_trt.cpp`** | 推理核心实现 | **YOLOv8 深度学习推理核心类**：<br>1. **前处理**：Letterbox 缩放算法（保持长宽比，双线性插值，自动填充灰边到 640×640）；<br>2. **显存流转**：通过 CUDA Stream 进行 Host 到 Device 的异步内存传输；<br>3. **硬件执行**：调用 TensorRT `IExecutionContext::enqueueV2` 执行 Volta GPU FP16 原生硬件推理；<br>4. **后处理**：解析 8400 个候选锚框，执行置信度初筛与高效 NMS（非极大值抑制）去重。 |
| **`main.cpp`** | 基准测试主入口 | **模型测速与效果验证主程序**：<br>1. 加载 FP16 引擎（`/data/models/yolov8n_fp16.engine`）与标签字典；<br>2. 读取输入图像（支持实拍图或全高清测试画幅）；<br>3. 执行推理并精确计算端到端毫秒耗时（实测 11.9ms）与 FPS（83.6 FPS）；<br>4. 在图像上绘制绿色检测框与类别置信度，导出核验图片 `annotated_result.jpg`。 |

---

## 核心接口说明 (`YOLOv8TRT` 类)

```cpp
// 1. 构造函数：指定 Engine 引擎路径与 labels 字典路径
YOLOv8TRT(const std::string& engine_path, const std::string& labels_path);

// 2. 初始化：读取 Engine 二进制反序列化，创建 CUDA 执行上下文与显存 Buffer
bool init();

// 3. 执行端到端推理：输入 OpenCV Mat 图像，输出目标检测结构体数组
std::vector<Detection> detect(const cv::Mat& image, float conf_thresh = 0.25f, float nms_thresh = 0.45f);
```
