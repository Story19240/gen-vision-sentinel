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

## 第三段：AI 参考火花片对拍（清单第 6 节，未通过并停止）

执行时间：2026-11-19T07:43:51Z（UTC）。环境同第一段：Debian 13 x86_64、Python 3.13.5、G++ 14.2.0、GNU Make 4.4.1；Python `cv2` 5.0.0，C++ OpenCV 4.10.0。AI 素材实测为 720×1280、24.0 FPS、声明 193 帧。仅使用 AI 片专用配置 `spark/prototype/local_ai720_config.json`，未使用 1#A 标定配置。

在仓库根目录原样执行：

```bash
python3 spark/prototype/run_offline.py \
  --input spark/videos/mmexport1793887748039.mp4 \
  --config spark/prototype/local_ai720_config.json \
  --output /tmp/spark_py_ai_reference

spark/src/bin/spark_offline \
  --input spark/videos/mmexport1793887748039.mp4 \
  --config spark/prototype/local_ai720_config.json \
  --output /tmp/spark_cpp_ai_reference

python3 spark/src/compare_offline.py \
  /tmp/spark_py_ai_reference \
  /tmp/spark_cpp_ai_reference
```

### 退出码、帧数与事件

- Python 退出码 0：输出 `processed 193 frame(s) at 24.000 FPS`，`total_frames=193`，`event_count=2`，指标数据 193 条。
- C++ 退出码 0：输出 `processed 193 frame(s) at 24.000 FPS`，`total_frames=193`，`event_count=2`，指标数据 193 条。
- compare 退出码 **1**，没有输出 `COMPARE_OK`；使用默认绝对容差 `1e-9`、相对容差 `1e-9`，未放宽。

| 端 | 事件 1 | 事件 2 |
| --- | --- | --- |
| Python | frame_index=92，frame_number=93，3833ms，heavy / blue_white | frame_index=113，frame_number=114，4708ms，heavy / blue_white |
| C++ | frame_index=93，frame_number=94，3875ms，heavy / blue_white | frame_index=113，frame_number=114，4708ms，heavy / blue_white |

历史参照差异：Python 复现了 2 个 heavy / blue_white 事件，但首事件为 3833ms，比历史约 3875ms 早 42ms（24 FPS 下 1 帧），第二事件 4708ms 一致。这是历史参照值差异；另有下述独立的 Python/C++ 不一致，不能写为通过。

### compare 差异证据

完整 compare 输出和两端原始运行输出保存在 `/tmp/spark_ai_compare_20261119.log`（2,207 行、155,037 字节，不提交）。对完整输出的逐行统计为：193/193 帧存在差异，共 2,084 条逐帧字段差异，另有 1 条事件列表差异和 12 条 `max_metrics` 差异。

| 逐帧差异字段 | 差异行数 | 首个差异帧/示例（Python != C++） |
| --- | ---: | --- |
| `color_match_ratio` | 193 | frame 0：0.8148148148148148 != 0.6153846153846154 |
| `contact_sat_ratio` | 193 | frame 0：0.0005113636363636364 != 0.0002462121212121212 |
| `contact_v_mean` | 193 | frame 0：128.66617424242423 != 127.42155303030303 |
| `falling_bright_pixels` | 193 | frame 0：243 != 196 |
| `reference_v_mean` | 193 | frame 0：185.40866738894908 != 185.30747562296858 |
| `rel_saturation` | 193 | frame 0：-0.00021994730621491178 != -0.00037675645950293837 |
| `contact_flicker` | 192 | frame 1：0.11537878787879663 != 0.11534090909090366 |
| `rel_flicker` | 192 | frame 1：-0.4851087527495963 != -0.47279560228504636 |
| `ref_flicker` | 190 | frame 1：0.6004875406283929 != 0.58813651137595 |
| `falling_components` | 165 | frame 1：14 != 16 |
| `ref_sat_ratio` | 130 | frame 0：0.0007313109425785482 != 0.0006229685807150596 |
| `color_hit` | 25 | frame 5：True != False |
| `candidate_duration_ms` | 19 | frame 88：42 != 0 |
| `candidate` | 8 | frame 87：True != False |
| `event_triggered` | 2 | frame 92：True != False |
| `level` | 2 | frame 92：heavy != none |
| `color_type` | 1 | frame 163：orange_yellow != blue_white |

`max_metrics` 的 12 项差异为：`candidate_duration_ms` 1083 != 750；`color_match_ratio` 0.8648648648648649 != 0.6885245901639344；`contact_flicker` 2.644903581267215 != 2.5804080578512325；`contact_sat_ratio` / `rel_saturation` 均为 0.17609848484848484 != 0.16460227272727274；`contact_v_mean` 171.49647727272728 != 169.51897727272728；`falling_bright_pixels` 7330 != 6585；`falling_components` 93 != 87；`ref_flicker` 0.9740544666600964 != 0.9780335861321781；`ref_sat_ratio` 0.000866738894907909 != 0.0008125677139761647；`reference_v_mean` 188.14804983748647 != 187.9687432286024；`rel_flicker` 2.2931873397622424 != 2.2289281996812322。

两端 `summary.json` 全文如下。

Python `/tmp/spark_py_ai_reference/summary.json`：

```json
{
  "total_frames": 193,
  "event_count": 2,
  "events": [
    {
      "frame_index": 92,
      "frame_number": 93,
      "timestamp_ms": 3833,
      "level": "heavy",
      "color_type": "blue_white"
    },
    {
      "frame_index": 113,
      "frame_number": 114,
      "timestamp_ms": 4708,
      "level": "heavy",
      "color_type": "blue_white"
    }
  ],
  "max_metrics": {
    "rel_saturation": 0.17609848484848484,
    "contact_sat_ratio": 0.17609848484848484,
    "ref_sat_ratio": 0.000866738894907909,
    "rel_flicker": 2.2931873397622424,
    "contact_flicker": 2.644903581267215,
    "ref_flicker": 0.9740544666600964,
    "color_match_ratio": 0.8648648648648649,
    "color_hit": 1,
    "falling_bright_pixels": 7330,
    "falling_components": 93,
    "candidate": 1,
    "event_triggered": 1,
    "contact_v_mean": 171.49647727272728,
    "reference_v_mean": 188.14804983748647,
    "candidate_duration_ms": 1083
  }
}
```

C++ `/tmp/spark_cpp_ai_reference/summary.json`：

```json
{
  "total_frames": 193,
  "event_count": 2,
  "events": [
    {
      "frame_index": 93,
      "frame_number": 94,
      "timestamp_ms": 3875,
      "level": "heavy",
      "color_type": "blue_white"
    },
    {
      "frame_index": 113,
      "frame_number": 114,
      "timestamp_ms": 4708,
      "level": "heavy",
      "color_type": "blue_white"
    }
  ],
  "max_metrics": {
    "rel_saturation": 0.16460227272727274,
    "contact_sat_ratio": 0.16460227272727274,
    "ref_sat_ratio": 0.00081256771397616469,
    "rel_flicker": 2.2289281996812322,
    "contact_flicker": 2.5804080578512325,
    "ref_flicker": 0.97803358613217806,
    "color_match_ratio": 0.68852459016393441,
    "color_hit": 1,
    "falling_bright_pixels": 6585,
    "falling_components": 87,
    "candidate": 1,
    "event_triggered": 1,
    "contact_v_mean": 169.51897727272728,
    "reference_v_mean": 187.96874322860239,
    "candidate_duration_ms": 750
  }
}
```

对拍产物 `/tmp/spark_py_ai_reference`、`/tmp/spark_cpp_ai_reference` 不提交。按任务单第 0 节的失败停止要求，本段记录后终止；未修改检测器代码、未修改阈值、未放宽容差，也未进入清单第 7 节。

## 第四段：标定配置兼容性（清单第 7 节，第 1 次继续执行，通过）

执行时间：2026-11-19T07:52:12Z（UTC）。第三段失败后已按要求停止并提交；用户在第 1 次继续中明确指示完成剩余任务，因此从最后提交段恢复，仅继续执行验证。第三段 AI 对拍失败结论保持不变，未修改检测器代码、阈值或比较容差。

恢复前执行 `git fetch origin`，退出码 0；`HEAD` 与 `origin/main` 均为 `0e6dac794131b225e9ac5a6bd543bd6e62655c55`，跟踪文件无修改。环境同第一段：Debian 13 x86_64、Python 3.13.5、G++ 14.2.0、GNU Make 4.4.1；Python `cv2` 5.0.0，C++ OpenCV 4.10.0。

在仓库根目录原样执行：

```bash
python3 spark/prototype/run_offline.py \
  --input spark/videos/mmexport1793887741382.mp4 \
  --config spark/prototype/results/real_baseline2/calibrated_config.json \
  --output /tmp/spark_py_baseline \
  --baseline

spark/src/bin/spark_offline \
  --input spark/videos/mmexport1793887741382.mp4 \
  --config /tmp/spark_py_baseline/calibrated_config.json \
  --output /tmp/spark_cpp_calibrated

python3 spark/src/compare_offline.py \
  /tmp/spark_py_normal_real \
  /tmp/spark_cpp_calibrated
```

- 退出码：Python `--baseline`=0，C++=0，compare=0。
- Python 基线：输出 `processed 349 frame(s) at 25.000 FPS`，`total_frames=349`，`event_count=0`，`events=[]`；生成 `/tmp/spark_py_baseline/calibrated_config.json`（1,634 字节，SHA256 `e35c2edb6d1365a87c619b27f7a28816d11a96cff28641a48c1c24a6f179f3ec`）。
- 新生成标定件包含 `_comment`、`camera`、`calibration` 等额外字段；C++ 正常退出并处理 349 帧，未因这些字段失败。
- 三类 ROI 均解析成功：`contact [110,240,390,395]`、`falling [140,395,380,640]`、`reference [640,130,1010,210]`；`sample` 与 `synthetic` 坐标一致，适配本次同一 1280×720 输入帧。
- 新标定件的 `rel_saturation=0.32302656670127305`、`flicker=8.192941815829471`、`confirm_duration_ms=200`，与历史标定配置一致；未跨相机套用。
- C++：输出 `processed 349 frame(s) at 25.000 FPS`，`total_frames=349`，`event_count=0`，`events=[]`，与第二段历史标定配置的 349 帧、0 事件一致；两端指标文件均为 349 条数据帧（含表头 350 行）。
- 逐帧与摘要比较的实际左操作数为第二段 `/tmp/spark_py_normal_real`，右操作数为 `/tmp/spark_cpp_calibrated`；标准输出仅为 `COMPARE_OK`，退出码 0。
- 浮点容差：未传覆盖参数，使用默认绝对容差 `1e-9`、相对容差 `1e-9`，未放宽。
- 原始日志：`/tmp/spark_calibrated_compare_20261119.log`；产物：`/tmp/spark_py_baseline`、`/tmp/spark_cpp_calibrated`，均不提交。

第四段验收通过。全部规定段落均已执行并记录；第三段 AI 对拍仍为已记录的失败结果，本报告未将其改写为通过。
