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
