# 火花检测器 C++ 移植环境矩阵

> 盘点日期：2026-11-08  
> 盘点范围：只读检查 `gen-vision-sentinel` 仓库、本云工作站及仓库内已有 Jetson 交付记录。  
> 证据口径：只有本次命令实测、仓库存档输出或明确源码路径才写“已确认”。无法从本机直接访问目标板、素材未入库或记录互相冲突的，写“未确认”。  
> 本文件不代表项目主已批准任何待确认项。

## 1. 结论摘要

| 项 | 实测结论 | 对移植的影响 |
| :--- | :--- | :--- |
| water C++ 编译机 | 仓库文档一致写明在 **Jetson AGX Xavier** 的 `/data/workspace/water/src/test` 或 `/data/workspace/water/src` 上构建；本云端工作站只有 `gen-vision-sentinel` 仓库，没有 OpenCV/GStreamer 工具链 | 以 **目标机本地编译** 为既定流程，本工作站不承担生产构建 |
| 构建系统 | water 是 **GNU Make + `g++`**，不是 CMake；生产 Makefile 用 C++14、`-O3` | spark 优先沿用 Make，不新增 CMake 依赖 |
| 编译器实测 | 本次工作站 `g++` 为 **14.2.0 x86_64**；Jetson 端仓库存档只记录 **GCC 9.4.0**，未远程复验 | Jetson 实际 g++ 版本：**未确认** |
| CMake 实测 | 本次工作站 `cmake` **未找到**；Jetson 端 CMake **未确认，且现有构建不需要** | 当前不用 CMake |
| 构建命令 | 生产：`cd water/src && make clean && make -j4`；测试工具：`cd water/src/test && make -j4`；一键运行见 `run_analyzer.sh` | spark 使用同等可读的 Make 命令 |
| 产物落点 | 生产 `water/src/water_monitor_server`；测试工具 `water/src/test/water_flow_analyzer`、`calibrate_roi`；注意 `test/Makefile` 虽同时链接客户端源文件，默认只产出 `water_flow_analyzer` | spark 二进制落在 `spark/src/bin/`，不跨目录覆盖 water 产物 |
| 目标 Jetson | Jetson AGX Xavier 16 GB，`tegra194`、载板 P2972；Ubuntu 20.04.6 LTS aarch64；L4T R35.6.5 / JetPack 5.1.x | 这些都来自仓库存档，**本次未直接登录板子复验** |
| 目标 OpenCV | 既有交付记录写 **OpenCV 4.2.0**；源码只使用兼容 4.x 的 core/imgproc/imgcodecs/videoio API。安装方式 **未确认** | README 直接运行原始 Python 原型或用 5.x 生成基线前，须先核对目标 OpenCV 版本 |
| 相机分辨率 | water 存档记录为 1920×1080；用户任务背景写 RTSP 主码流 2560×1440；仓库没有可直接核验 1#A 主码流配置的实时输出 | 两套尺寸信息冲突，实际主码流尺寸 **未确认**；ROI 上线前必须肉眼核对 |
| RTSP | water 有 GStreamer/OpenCV RTSP 实现，可借鉴；其 NVDEC 管线写死 H.265 和 BGRx 格式，不能无条件直接复用 | spark 独立进程可后续抽公共拉流，但离线一期不需要新增依赖 |
| JSON | 生产 C++ 无标准 JSON 库，多用 `std::regex`/字符串提取，不能当解析器直接复用 | spark 离线版加入仓库内小型严格 JSON 解析器，不新增系统依赖 |
| 飞书 | `water/src/feishu_client.{h,cpp}` 有可复用 HTTP 发送骨架；水情专属卡片接口不能直接用于火花告警 | 一期可接同模块飞书底层，但需新增 spark 卡片接口；本期只做离线检测 |
| 继电器 | `water/src/relay_client.{h,cpp}` 有 Modbus TCP 客户端 | 按工作假设一期不直连 Y1，**待项目主最终确认** |
| 曝光/增益 | 仓库 C++ 源码和配置中 **未发现 AE/AGC/曝光/增益锁定实现或参数** | 须在相机侧锁定并另行核对；不能宣称已实现 |
| ROI 缩放 | water 后端 **没有**按 720p→1440p 自动缩放；裁剪使用原始像素，Web 标定仅在显示 canvas 与原始图像之间换算 | 1#A 在 720p 上画的坐标不能直接假设适用于 1440p |

## 2. 逐项实测

### 2.1 water C++ 编译位置、工具链、命令和产物

**实测结论：已确认来源机器是 Jetson AGX Xavier；编译器和 OpenCV 版本来自仓库存档；本次云端工作站没有可执行的生产构建环境。**

| 项目 | 结论 | 实测依据 | 状态 |
| :--- | :--- | :--- | :--- |
| 编译机器 | Jetson AGX Xavier 16 GB，路径 `/data/workspace/water` | `water/src/README.md`、交接文档、`全局项目文档.md`；本机 `/data/workspace` 下无 `water` 目录 | 已确认来源；当前远程不可复核 |
| 云端工作站 | Debian 13，x86_64，4 vCPU / 8 GiB；无 `/usr/include/opencv4`、无 OpenCV/curl 开发包 | `MACHINE.md`、`uname`、头文件检查 | 已确认 |
| 编译器 | 板端存档 `GCC 9.4.0`；生产 Makefile 变量 `CXX = g++` | `docs/Jetson 开发板环境说明.md`、`water/src/Makefile` | 板端实测值已存档；本次未复验 |
| 语言标准 | C++14 | `water/src/Makefile`、`water/src/test/Makefile` 的 `-std=c++14` | 已确认 |
| CMake | 现有 water 构建未使用 CMake；本次工作站未找到；板端 CMake 版本未确认 | 本次 `command -v cmake` 无结果 | 未确认 |
| 生产构建命令 | `cd /data/workspace/water/src && make clean && make -j4` | Makefile 和 README | 已确认 |
| 测试工具构建命令 | `cd /data/workspace/water/src/test && make clean && make -j4` | `water/src/test/README.md` | 已确认 |
| 一键运行脚本 | `cd /data/workspace/water/src/test && bash run_analyzer.sh` | `run_analyzer.sh` 先切目录后执行 `make -j4` 和 `./water_flow_analyzer` | 已确认脚本行为 |
| 生产产物 | `/data/workspace/water/src/water_monitor_server` | `water/src/Makefile` 的 `TARGET_SERVER` | 已确认 |
| 测试产物 | `/data/workspace/water/src/test/water_flow_analyzer` | `water/src/test/Makefile` 的 `TARGETS` | 已确认 |
| 标定工具产物 | `water_flow_analyzer` 目标规则可产出 `calibrate_roi`；但默认 `run_analyzer.sh` 不产生它 | Makefile 有规则，`all` 仅有 `water_flow_analyzer` | 已确认 |
| water_config 搜索路径 | 测试进程向上两级找 `water_config.json`，但 `water_config.example.json` 在 `water/` 下一级；只有把示例复制为 `water/water_config.json` 时命中第三候选 | `water_flow_analyzer.cpp:308-317` 与仓库目录 | 已确认 |
| 实际板端已编译证据 | 仓库没有二进制、构建日志或本次 SSH 会话输出 | `git ls-files` 无产物；仓库不是 `/data/workspace/water`，不据此推断板端文件状态 | 未确认 |
| 目标板连通性 | 本次从云工作站执行 `ssh root@192.168.1.100`，端口 22 返回 **Connection refused** | 本次 SSH 实测 | 不可从本机代跑板端构建 |
| 现有交付格式 | 没有 CMakeLists；有 GNU Makefile | 仓库文件列表 | 已确认 |

补充证据与环境边界：

- `water/src/README.md` §“正式服务编译与运行指南 (Jetson AGX Xavier)”明确给出 SSH 到 `192.168.1.100`、进入 `/data/workspace/water/src` 后 `make clean && make -j4`。
- `water/src/test/run_analyzer.sh` 是一只包含 `cd /data/workspace/water/src/test` 的板端脚本；其中的绝对路径不能在本云工作站直接执行。
- Jetson 系统档记为 Ubuntu 20.04.6 LTS / L4T R35.6.5 / Kernel 5.10.216-tegra；环境说明另有段落将 `g++/cmake` 标为“尚未验证”，因此只能确认构建配置，不把“本次板端重新编译成功”写成已确认。
- 云端 `g++ 14.2.0` 不能代替 Jetson `GCC 9.4.0` 的版本对拍结论；离线 C++ 的构建和对拍需在项目主本地或目标板执行。

### 2.2 目标 Jetson 和 OpenCV

| 项目 | 实测结论 | 证据 | 确定性 |
| :--- | :--- | :--- | :--- |
| 型号 | NVIDIA Jetson AGX Xavier，16 GB | 环境说明记录 `tegra194`、`Jetson-AGX`、P2972、约 15 GB 内存 | 仓库存档已确认；本次未联网复验 |
| OS | Ubuntu 20.04.6 LTS，aarch64 | 环境说明、全局项目文档 | 仓库存档已确认 |
| L4T / JetPack | R35.6.5 / JetPack 5.1.x 系列 | 同上 | 仓库存档已确认 |
| GCC | 9.4.0 | 环境说明 | 仓库存档已确认 |
| CUDA / TensorRT | 11.4 / 8.5.2.2 | 全局项目文档 | 与离线火花检测无直接依赖 |
| OpenCV | 4.2.0，C++ `core`、`imgproc`、`imgcodecs`、`dnn` | `全局项目文档.md` §2.1 | 仓库存档已确认 |
| OpenCV 安装方式 | **未确认** | 仓库仅记录版本；没有 apt、源码或 JetPack 组件安装证据 | 未确认 |
| 实际板端 OpenCV | 版本和安装布局均 **未确认** | 本次 SSH 22 拒绝；未运行 `OpenCVConfig` / `cv2` / `pkg-config` 查询 | 未确认 |
| Spark 所需 videoio | water 测试 Makefile 链接 `opencv_videoio`；目标 OpenCV 是否具备可用视频后端及 FFmpeg/GStreamer 支持 | 能推断库组件在，不能推断编解码能力 | 未确认 |

### 2.3 RTSP、JSON、飞书现成代码和复用判断

| 能力 | 现成文件 | 实测行为 | 能否直接复用 |
| :--- | :--- | :--- | :--- |
| RTSP / GStreamer 硬解 | `water/src/water_monitor_server.cpp:167-173`，`water/src/test/water_flow_analyzer.cpp` 主入口 | water 生产服务用 `rtspsrc → rtph265depay → nvv4l2decoder → nvvidconv → BGR appsink`；输出按原分辨率 | **部分可复用**；需抽公共模块并处理 H.264/H.265、主/子码流、源尺寸；不能只复制写死管线 |
| RTSP / OpenCV VideoCapture | `water/src/test/calibrate_roi.cpp` 输入源分支 | 标定工具用 `VideoCapture` 打开网络流/文件 | **可借鉴**，不用于正式低延迟保证 |
| 环镜读取 `.env` | `water/src/env_utils.h` | 读取固定路径与当前目录 `.env`，每次 `env_or` 调重载；不解析转义、引号和注释细节 | **可参考**；“直接复用”需限定简单键值场景 |
| JSON 读写 | `water/src/test/water_flow_analyzer.cpp` 的 `std::regex`、字符串提取和手拼 `get_status_json/save_config`；`water/scripts_py/*.py` 使用 Python `json` | C++ 正则依赖字段顺序/文本形状，不是完整 JSON 解析；存在转义、嵌套对象、格式变体隐患 | **不可直接复用为 spark C++ 配置解析器** |
| 飞书 HTTP 发送 | `water/src/feishu_client.h`、`water/src/feishu_client.cpp` | libcurl `POST`、JSON 请求头、6 s 超时、代理环境/局域网 10811 探测；`send_card_async` 独立 `std::thread` 后 detach | **发送骨架可复用**；线程生命周期、JSON 转义、水情专用卡片接口需整改，不能整份只改文案 |
| 飞书 Token | 同一组 `feishu_client.*` | 后台预取 tenant token；获取时用字符串查找 | **可参考**，需纳入 spark 独立客户端或抽取公共组件 |
| 飞书 Python | `alarm_py/feishu_alarm.py` | 有 `notify_spark_alarm` 等路径 | 不属于 C++ 可直接复用；可作告警字段参考 |
| Modbus 继电器 | `water/src/relay_client.h/.cpp` | C++ Modbus TCP 驱动，Y 通道映射在 water 业务里 | 一期记录仪范围不接；不复制 water 业务决策 |
| Web Studio | `water/src/test/calibrate_studio.html`、`water/src/test/water_flow_analyzer.cpp` | Canvas 缩放、鼠标像素→原图坐标、MJPEG/状态接口 | **启发可复用**，不是 spark ROI 标定成品；一期离线版不包含 Web 服务 |

补充（2026-11-08 只读复核发现，本次未改 water 业务代码）：

- `feishu_client.cpp` 的卡片构建代码在 `"type"` 附近把发送按钮字段重复插入了两次；spark 复用发送底层时应自行组装一次且合法的 JSON，不照搬该片段。
- 源码中 RTSP 主入口实际使用 `nvvidconv` 和 `protocols=tcp`；`water/src/test/README.md` 对“GPU 直入、CPU 低于 15%”的性能结论是历史记录，本次未复测。
- 本机 OpenSSH 实际版本为 `OpenSSH_10.0p2 Debian-7+deb13u4`（`ssh -V` 实测）；该版本与目标 Jetson 端服务配置无关。

### 2.4 spark C++ 位置与 water 共存方式

**建议：新增 `spark/src/`，不修改 `water/src/` 的业务文件；spark 使用独立 Makefile 和独立二进制，后续只抽取可共享的纯客户端代码。**

设计文档已将 `spark/src/` 列为预留位置（`docs/碳刷火花监测模块设计文档.md` §5.1），但本次检查时该目录尚不存在。这与现有 `spark/prototype/` 的 Python 原型、`water/src/` 的已交付服务划分一致。

| 项目 | 建议 |
| :--- | :--- |
| 离线版源码 | `spark/src/` |
| 构建文件 | `spark/src/Makefile`，产物放 `spark/src/bin/spark_offline` |
| 配置 | 继续使用 `spark/spark_config.example.json`；`calibrated_config.json` 可直接传入 |
| 图片 | `spark/images/` |
| 视频/逐帧素材 | `spark/videos/`，大文件按现有 `.gitignore` 另存 |
| 离线结果 | 请求的输出目录；仓库只提交文本 JSON/CSV，不提交大视频 |
| 生产服务预留 | 后续 `spark/src/spark_monitor_server.cpp`，确认端口后再定 |
| water 共存 | 各目标和进程独立；不链接 `water_flow_analyzer.o`，不共享全局状态；公共底层将来抽目录后两边引用 |

**工作假设（逐项待项目主最终确认）：**

1. 判定口径统一毫秒：持续 `>=200ms` 才上报一次。离线 CLI 按与 Python 相同的逐帧名义时间推进，不擅自改成墙钟时间。
2. 进程形态：先独立隔离。离线版是单独 CLI；生产一期候选为独立记录仪进程，Web 端口避开 water 的 8080。
3. Y1 一期只记录 + 飞书告警，不直连跳闸。离线版不产生任何继电器动作。
4. 相机源为 RTSP，与 water 同一套。离线版不接 RTSP；生产接入时复用 water 的 GStreamer 取流思路，并先核对实际分辨率/编码。
5. water 没有 spark 专用颜色/ROI 算法；算法必须严格翻译 `spark/prototype/spark_detector.py`，不要把 water 的帧差阈值当作火花阈值。

### 2.5 ROI 坐标与分辨率

**实测结论：water 没有归一化或 720p→1440p 自动缩放约定。**

| 检查项 | 结论 | 依据 |
| :--- | :--- | :--- |
| 后端缩放 | water 未发现 resize 源流/ROI 到固定 720p/1080p | `water/src/*.cpp`、`water/src/test/*.cpp` 裁剪都在原帧 |
| ROI 格式 | `[x1,y1,x2,y2]` 像素坐标，原图空间 | `cv::Rect(x1,y1,x2-x1,y2-y1)` |
| 归一化 | 未发现 | 不填 0~1，不支持其他标定配方 |
| Web 绘制/回写 | 仅在 canvas 显示尺寸与图像 `naturalWidth/naturalHeight` 间换算；回写仍是画布像素坐标 | `water/src/test/water_flow_analyzer.cpp:1129-1149、1169-1174` |
| 超出画面处理 | 按原图范围裁剪；完全出画面则退化为空 ROI/0 能量 | `water_flow_detector.cpp:109-116`、`water_flow_analyzer.cpp:696-699` |
| `calibrated_config.json` | `spark/` 与 Git 历史中 **未找到**，原型输出目录未入库 | 未可用；不得声称已核验 |
| 1#A 真实录像 | 未入库；原型只记录 1280×720、25fps、349 帧 | 无法在本机重放确认 ROI |
| 2560×1440 素材 | 未找到 | 不能据此给出对拍结论 |

**720p 坐标用于 2560×1440 主码流的风险：** 若原样使用，roi 会落在左上约四分之一区域。因为 water 不自动缩放，roi 不会透明适配，也不会因越界而被静默 remove；它会在错误位置继续产生亮度/颜色/连通域统计的那个左上区域。

**上线前必做（不能由代码静默代劳）：**

1. 确认 1#A 的真实 RTSP URL 与主/子码流，记录相机后台的分辨率、编码、帧率。
2. 若主码流确实是 2560×1440，在 **该主码流** 抓图或录像中重新标定三个 ROI，不把 720p 坐标直接乘 2 当定案。
3. 若还要在 720p 与 1440p 之间共用人工标定，由标定工具显式给两路对应点求射影映射并保存坐标空间元数据；该功能**本期未实现**。
4. 标定后肉眼确认接触线、参考区和下落区均框住正确部位，再在同一相机正常录像上回跑 0 误报，并用参考火花片跑级别对拍。

### 2.6 曝光与增益锁定

| 位置 | 实测结论 |
| :--- | :--- |
| `water/src/` C++ | 未发现 `CAP_PROP_EXPOSURE`、`CAP_PROP_GAIN`、V4L2 曝光/增益设置或相机 SDP/ONVIF 控制代码 |
| `water_config.example.json` | 未发现曝光/增益字段 |
| `.env.example` | 只有 RTSP 地址和飞书变量 |
| 设计文档 | 只把“固定 AE/AGC、写入 spark 配置”列为要求与待核实项，不是已实现事实 |
| `spark_config.example.json` | `camera.exposure_us`、`camera.gain_db` 为 null，注明待现场填写；离线检测器不读取 |
| 1#A 实际相机 | 是否支持手动锁定、实际锁定值：**未确认** |

**结论：** water 侧没有可直接复用的曝光/增益锁定实现，也没有相应配置项。此项必须先通过相机后台或 ONVIF 完成，再记录到 spark 配置；不能把字段存在误写成相机已锁定。

## 3. 素材与对拍资格

| 素材 / 产物 | 仓库状态 | 可下结论 |
| :--- | :--- | :--- |
| `spark/images/carbon_brush_sample.png` | 已在仓库 | 可跑 C++ 对照 Python；0 事件不足以证明火花灵敏度 |
| 蓝白/橙黄合成场景 | Python 生成器在仓库；`spark/prototype/results/` 未入库 | 用相同场景、种子、fps、帧数现场生成后可直接对拍；当前没有旧结果可比对 |
| 1#A 正常录像 1280×720 / 25fps / 349 帧 | 未入库 | 无法在本工作站复现 0 误报；只能记录项目主历史复验结论 |
| 1#A 正常录像基线 / `calibrated_config.json` | 未入库，Git 历史亦没有 | 不能确认“真实 1#A 标定件可直接喂 C++”；只能验证 Python 格式生成的标定配置兼容性 |
| AI 生成参考火花片 720×1280 / 24fps / 193 帧 | 未入库；README 和验证清单只记录 2 次事件、3.875 s、4.708 s | **不能**声称 C++ 与旧 Python 逐帧一致；只能如实记录缺文件 |

## 4. 移植方案要点

1. **算法翻译**：只翻译 Python 现有的四路信号和状态机；保留 Python 的帧时间口径、ROI 配置包含关系、8 邻域连通域、`>=` 阈值和事件落锁行为。不对旧算法做“顺手优化”。
2. **离线输入输出**：支持视频与图片输入、`--fps` 覆盖、合成场景、种子、帧数、`--baseline`；输出 `metrics.csv`、`summary.json`、合成/图片 `annotated.png`、可选 `baseline.json` / `calibrated_config.json`。
3. **配置和错误**：配置解析器只接受严格 JSON 对象；缺少或非法 `contact_roi`/`reference_roi`/`falling_roi` 在写结果前报错并退出 2；`calibrated_config.json` 结构可直接传入。
4. **直接对拍**：C++ 与 Python 对同一素材分别输出 metrics，再按 `frame_index`、各字段、事件列表、等级和配色比较；浮点先按统一小误差，后追足位。
5. **复用边界**：离线版不链接 water 源文件；检测核心不依赖 OpenCV，完整图片/视频读取使用 OpenCV 4.x 兼容 API，不引入 curl。生产接入时再抽流和飞书公共模块。复用不改变现有 water 进程和端口。
6. **工作假设标识**：所有按默认先行的设计项都写“待项目主最终确认”；离线实现不因此扩成生产系统。

## 5. 工具链结论与遗留

| 环境 | 实测 | 能否在此完成 |
| :--- | :--- | :--- |
| 云工作站 | GCC 14.2.0、Make 4.4.1、Python 3.13.5；无 OpenCV、curl 开发头、OpenCV Python 模块、CMake | 不能编译 C++ 或运行 Python 真值；可做静态自查 |
| 目标 Jetson | 存档 GCC 9.4.0 / Ubuntu 20.04 / OpenCV 4.2.0；本次远程 22 被拒 | 未在当前会话上场 |
| 项目主本地 | `待验证清单.md` 记录 Python 3.10.22 / OpenCV 5.0.0 / numpy 2.2.6；C++ 工具链 **未确认** | 按清单现场补测并完成对拍 |

按“工作站只编写，工具链在本地”约定，不在本机安装 OpenCV、GStreamer、curl 开发包或任何重型工具链。C++ 构建与逐帧对拍作为待本地验证项随代码交付。
