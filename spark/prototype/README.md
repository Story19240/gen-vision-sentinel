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

退出码 `0` 表示 7 个算法场景和 1 个坏配置拒绝检查全部满足预期；非零退出码表示至少一项未通过。每项检查会打印 `PASS` 或 `FAIL`。坏配置检查要求 `run_offline.py` 返回非零码，并在 stderr 明确指出无效 ROI，同时不得生成 `metrics.csv` 或 `summary.json`。

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

## 基线标定工作流

阈值必须针对每一台相机分别标定，不能把一台相机生成的阈值直接套到另一台相机。完整流程如下：

1. 固定目标相机的曝光、增益和白平衡，使用该相机导出的正常无火花录像建立基线。
2. 对正常录像运行 `--baseline`，输出目录会同时得到 `baseline.json` 和可直接使用的 `calibrated_config.json`。
3. 检查 `calibrated_config.json` 中的 ROI 与标定来源，再用它重跑同一录像或其他同相机素材。

```bash
conda activate gen-vision-sentinel
python spark/prototype/run_offline.py \
  --input /path/to/camera-a-normal.mp4 \
  --config spark/spark_config.example.json \
  --output spark/prototype/results/camera-a-baseline \
  --baseline

python spark/prototype/run_offline.py \
  --input /path/to/camera-a-normal.mp4 \
  --config spark/prototype/results/camera-a-baseline/calibrated_config.json \
  --output spark/prototype/results/camera-a-calibrated
```

`calibrated_config.json` 以输入配置为底，只替换以下三项：

- `thresholds.rel_saturation`：正常基线 `mean + 5σ`。
- `thresholds.flicker`：正常基线 `mean + 5σ`。
- `level_thresholds.heavy_bright_pixels`：正常基线 `mean + 5σ` 向上取整。

其余配置原样保留，并附带 `calibration` 字段记录三项指标的 mean、std 和建议值，便于追溯。

ROI 标定要点：

- 接触区 ROI 贴紧刷环与碳刷的实际接触线，不要圈入大面积发亮环面，否则饱和门会长期开启。
- 下落区 ROI 必须避开常亮金属件，否则 `falling_bright_pixels` 基线被抬高，重级阈值失真。
- 参考区选择与接触区同视场、同材质倾向的静止表面，用于抵消共同光照漂移。
- 标定完成后将生成的 `calibrated_config.json` 按相机归档；相机位置、曝光、增益或 ROI 改变后必须重新标定。

## 文件

- `spark_detector.py`：四路物理信号检测器与事件状态机。
- `synthesize.py`：确定性背景、合成火花和七种验证场景。
- `self_test.py`：七场景断言与坏配置拒绝检查。
- `run_offline.py`：图片、视频、合成场景和正常基线离线 CLI。
- `待验证清单.md`：项目主本地电脑的验证命令与验收标准。
