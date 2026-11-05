# 火花监测离线算法原型

本目录用于验证碳刷火花检测的四路物理信号逻辑：接触区相对饱和率、相对闪烁、颜色签名，以及下落区亮点分级。原型使用 Python，后续确认算法成立后另行移植到生产 C++。

本目录不包含相机、RTSP、飞书、继电器或生产服务实现。设计文档第六章中的相机源、进程形态和 Y1 动作方式仍待项目主确认；`confirm_duration_ms=200` 是当前原型默认值，也待项目主确认。

## 环境

在项目主的本地电脑使用 conda 环境运行，不要在云端工作站安装依赖。

```bash
cd /path/to/gen-vision-sentinel
conda env list
conda create -n gen-vision-sentinel python=3.11 -y
conda activate gen-vision-sentinel
python -m pip install -r spark/prototype/requirements.txt
```

如果 `gen-vision-sentinel` 环境已经存在，跳过 `conda create`，直接激活即可。安装后记录实际环境版本：

```bash
python --version
python -c "import cv2, numpy; print('opencv-python-headless', cv2.__version__); print('numpy', numpy.__version__)"
```

## 自验证

在仓库根目录运行：

```bash
conda activate gen-vision-sentinel
python spark/prototype/self_test.py
```

退出码 `0` 表示 7 个场景全部满足预期；非零退出码表示至少一个场景未通过。每条场景会打印 `PASS` 或 `FAIL`。

## 离线运行

样本图片：

```bash
conda activate gen-vision-sentinel
python spark/prototype/run_offline.py \
  --input spark/images/carbon_brush_sample.png \
  --config spark/spark_config.example.json \
  --output spark/prototype/results/sample_image
```

蓝白火花合成序列：

```bash
conda activate gen-vision-sentinel
python spark/prototype/run_offline.py \
  --scenario blue_white_spark \
  --config spark/spark_config.example.json \
  --output spark/prototype/results/blue_white_spark \
  --fps 25 \
  --frames 60 \
  --seed 20261105
```

橙黄火花合成序列：

```bash
conda activate gen-vision-sentinel
python spark/prototype/run_offline.py \
  --scenario orange_spark_falling \
  --config spark/spark_config.example.json \
  --output spark/prototype/results/orange_spark_falling \
  --fps 25 \
  --frames 60 \
  --seed 20261105
```

正常场景基线统计：

```bash
conda activate gen-vision-sentinel
python spark/prototype/run_offline.py \
  --scenario normal \
  --config spark/spark_config.example.json \
  --output spark/prototype/results/baseline \
  --fps 25 \
  --frames 60 \
  --seed 20261105 \
  --baseline
```

每次运行都会写入 `metrics.csv` 和 `summary.json`。图片或合成场景还会生成带三类 ROI 和事件标注的 `annotated.png`；基线模式额外写入 `baseline.json`，其中包含 `rel_saturation`、`rel_flicker`、`falling_bright_pixels` 的 mean、std、P99.9，以及 `mean + 5σ` 建议阈值。

## 文件

- `spark_detector.py`：四路物理信号检测器与事件状态机。
- `synthesize.py`：确定性背景、合成火花和七种验证场景。
- `self_test.py`：七场景断言自验证。
- `run_offline.py`：图片、视频、合成场景和正常基线离线 CLI。
- `待验证清单.md`：项目主本地电脑的验证命令与验收标准。
