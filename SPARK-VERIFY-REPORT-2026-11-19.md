# 火花 C++ 真实素材对拍验证报告（2026-11-19）

## 范围与判定口径

- 按 `ORDER-2026-11-19-spark-real-video-compare.md` 执行清单第 5、6、7 节，不重跑第 1～4 节。
- 仅验证和记录，不修改检测器代码或阈值；各段完成后分别提交并推送。
- Python/C++ 对拍以同一素材、同一配置及 `spark/src/compare_offline.py` 的结果为准；历史结果仅作复现参照。

## 启动预检（已提交本段后进入构建）

执行时间：2026-11-19T07:33:33Z（UTC）。工作目录为 `/data/workspace/gen-vision-sentinel`，当前会话 ID 为 `01a1f2b7-f89d-7793-8f15-841cf1c5992b`；`/data/home/.codex/config.toml` 中 `model = "kimi-k3.1-pro"`，与任务单要求一致。

执行 `git fetch origin`，退出码 0。当前分支为 `main`，`HEAD` 与 `origin/main` 均为 `b3113cda21e62fc6ee7220b28ad9945dc4b5fdb3`，提交主题为 `docs(spark-cpp): 补充离线移植构建与对拍清单`。跟踪文件无修改、暂存区为空；未跟踪项仅为本任务单和调度方提供的本地配置/结果素材，视频受 `.gitignore` 排除。后续提交只显式暂存报告和验证清单，不暂存本地素材。

五项素材均位于任务单指定路径，实测字节数与 SHA256 如下，全部一致：

| 素材 | 字节数 | SHA256 |
| --- | ---: | --- |
| `spark/videos/mmexport1793887741382.mp4` | 2,634,499 | `64f9a26aae23c9c428df22f6076f91e33b30da653182a85a06de8786af6677ec` |
| `spark/videos/mmexport1793887748039.mp4` | 741,912 | `971bbd50fdc551472e65c283acd8c80b7a95d72bbf278c2f275b608f4e1e7cc4` |
| `spark/prototype/results/real_baseline2/calibrated_config.json` | 1,722 | `822114296a46ad9234a78ae63483c28b062815fcd8a07fe8098d5a5b884a70b2` |
| `spark/prototype/local_ai720_config.json` | 783 | `470749304d950f49ea5a6d7faf443223f525ce8829af736d2d66c98f7c98e3f4` |
| `spark/prototype/local_real720_config.json` | 836 | `87a6330bf5de3c9c32a73b853053e383a449f698bc2405b4dbfbf4c3bea38fce` |

预检环境：Debian GNU/Linux 13 (trixie)，x86_64；Python 3.13.5；G++ 14.2.0；GNU Make 4.4.1；`pkg-config` 报告 OpenCV 4.10.0；`libopencv-dev 4.10.0+dfsg-5` 通过系统软件包安装且状态为 `install ok installed`；`cmake` 未安装（本单使用 Make，不需要 CMake）。

预检结论：通过，可以进入完整媒体版构建。

## 第一段：完整媒体版构建（清单第 2 节）

执行时间：2026-11-19T07:38:15Z（UTC）。在 `/data/workspace/gen-vision-sentinel/spark/src` 原样执行：

```bash
pkg-config --modversion opencv4
make clean
make OPENCV_PKG=opencv4
```

- 退出码：`pkg-config`=0（输出 `4.10.0`），`make clean`=0（输出 `rm -rf bin`），`make OPENCV_PKG=opencv4`=0。
- 构建机：`instabox-box-f56ce89d`，Debian GNU/Linux 13 (trixie)，x86_64；G++ 14.2.0（Debian 14.2.0-19），GNU Make 4.4.1，Python 3.13.5；CMake 未安装，本段未使用。
- OpenCV：4.10.0，安装方式为 Debian 系统包 `libopencv-dev 4.10.0+dfsg-5`，`dpkg` 状态 `install ok installed`；编译参数包含 `-I/usr/include/opencv4`，产物动态链接 `libopencv_videoio.so.410`、`libopencv_imgcodecs.so.410`、`libopencv_imgproc.so.410`、`libopencv_core.so.410`。
- 完整 warning/error：无。`make OPENCV_PKG=opencv4` 仅输出一条 `g++ -std=c++14 -O3 -Wall -Wextra -pedantic` 编译链接命令，编译器未发出 warning 或 error；完整原始构建日志保存在本机 `/tmp/spark_build_20261119.log`，不提交。
- 产物：`spark/src/bin/spark_offline` 已生成，226,024 字节，x86-64 ELF 可执行文件，退出码检查为 0。

第一段验收通过，未修改检测器代码或阈值。

## 第二段：真实正常录像对拍（清单第 5 节）

执行时间：2026-11-19T07:40:27Z（UTC）。素材 `spark/videos/mmexport1793887741382.mp4` 与历史标定配置 `spark/prototype/results/real_baseline2/calibrated_config.json` 均按预检路径使用；Python `cv2` 5.0.0 读取到 1280×720、25.0 FPS、声明 349 帧。C++ 使用上述 OpenCV 4.10.0 完整媒体版。

在仓库根目录原样执行：

```bash
python3 spark/prototype/run_offline.py \
  --input spark/videos/mmexport1793887741382.mp4 \
  --config spark/prototype/results/real_baseline2/calibrated_config.json \
  --output /tmp/spark_py_normal_real

spark/src/bin/spark_offline \
  --input spark/videos/mmexport1793887741382.mp4 \
  --config spark/prototype/results/real_baseline2/calibrated_config.json \
  --output /tmp/spark_cpp_normal_real

python3 spark/src/compare_offline.py \
  /tmp/spark_py_normal_real \
  /tmp/spark_cpp_normal_real
```

- 退出码：Python=0，C++=0，compare=0。
- Python：输出 `processed 349 frame(s) at 25.000 FPS`，`total_frames=349`，`event_count=0`，`events=[]`；`metrics.csv` 为 349 条数据帧（含表头 350 行）。
- C++：输出 `processed 349 frame(s) at 25.000 FPS`，`total_frames=349`，`event_count=0`，`events=[]`；`metrics.csv` 为 349 条数据帧（含表头 350 行）。
- 历史复现：Python 的 `max_metrics.rel_saturation=0.22361751152073733`、`max_metrics.rel_flicker=5.087411329951655`，与历史约 0.2236、5.0874 一致；349 帧 0 事件亦一致。
- 逐帧与摘要比较：标准输出仅为 `COMPARE_OK`，退出码 0；帧数、事件列表、事件数及受检指标全部通过。
- 浮点容差：未传覆盖参数，使用比较器默认绝对容差 `1e-9`、相对容差 `1e-9`，未放宽。
- 原始日志：`/tmp/spark_real_compare_20261119.log`；对拍产物：`/tmp/spark_py_normal_real`、`/tmp/spark_cpp_normal_real`，均不提交。

第二段验收通过，未修改检测器代码或阈值。
