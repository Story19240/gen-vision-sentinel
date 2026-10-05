# 冷却水系统 Python 探针与工具脚本 (`water/scripts_py`)

本目录存放用于发电机冷却水网络摄像机连通性排查、RTSP 握手、SDP 参数协商等轻量级 Python 调试脚本。

---

## 脚本列表与说明

| 文件名 | 适用环境 | 说明 |
| :--- | :--- | :--- |
| **`check_rtsp.py`** | Python 3 | **工业相机 RTSP 连通性快速探针**：基于原生 `socket` 直连海康摄像机 `192.168.1.64:554`，发送 `DESCRIBE` 指令并打印相机的完整应答报文，用于 1 秒内诊断网线是否插好、账号密码是否正确。 |
| **`get_sdp.py`** | Python 3 | **码流 SDP 媒体参数提取脚本**：深度解析相机 RTSP 流的 SDP 报文，提取当前的视频编码格式（H.264 / H.265）、SPS/PPS 参数、时钟基准与音频通道配置，为硬解码器配置提供依据。 |
| **`calibrate_roi_local.py`** | Windows 本地 | **Windows 桌面交互标定工具**：纯本地 OpenCV 弹窗画框，支持按键保存最新坐标至 `water_config.json`。 |
| **`water_visual_inspector.py`**| Windows 本地 | **离线动能可视化与演练工具**：用于本地离线回放 MP4 视频、分析水流帧差动能与状态机流转。 |

---

## 常用运行指令

### 1. 验证摄像机网络与认证连通性
```bash
python3 check_rtsp.py
```
* **预期输出**：收到 `RTSP/1.0 200 OK`，并附带相机的 SDP 媒体描述。
* **异常排查**：
  * 若报 `Connection refused` 或超时：检查网线与开发板网口 IP（`192.168.1.100`）；
  * 若报 `401 Unauthorized`：检查本地安全配置中的用户名密码。

### 2. 提取并分析码流 SDP 参数
```bash
python3 get_sdp.py
```
* **输出内容**：打印当前摄像机的编码类型（H.264 / H.265/HEVC）、分辨率（1920×1080）与帧率参数。
