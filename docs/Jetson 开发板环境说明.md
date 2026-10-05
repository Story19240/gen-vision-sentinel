# Jetson 开发板环境说明

验证日期：2026-10-02 ｜ 用途：摄像头取流 → TensorRT 推理（YOLOv8 目标检测）→ C++ 判断逻辑

> 勘误：前面对话里我把这块板子说成 Xavier NX，这是错的。根据实测输出（`tegra194`、`Jetson-AGX`、`MODE_15W_DESKTOP`、约 15 GB 内存、载板型号 P2972），它是 **Jetson AGX Xavier（16 GB）**。性能比 NX 更强，下文以实测为准。

## 1. 硬件

| 项目 | 内容 |
| --- | --- |
| 平台 | Jetson AGX Xavier 开发套件（SoC 家族 tegra194，载板 P2972，板标 945-82972-0006-000） |
| 内存 | 14887 MB 可用（16 GB 版），Swap 7443 MB |
| CPU | 8 核 NVIDIA Carmel（ARM64），当前电源模式只开 4 核（cpu0–3），最高 2188.8 MHz |
| GPU | Volta 架构，当前模式最高 675.75 MHz |
| 加速器 | 2× DLA、2× PVA（硬件存在，当前未使用） |
| 存储 | 未采集（待执行 `df -h /`） |

## 2. 软件版本（实测）

| 组件 | 版本 | 备注 |
| --- | --- | --- |
| 操作系统 | Ubuntu 20.04.6 LTS（aarch64） | 精简镜像 |
| L4T | R35，REVISION 6.5（`/etc/nv_tegra_release`） | JetPack 5.1.x 系列 |
| 内核 | 5.10.216-tegra |  |
| CUDA | 11.4（nvcc 11.4.315，编译于 2022-10-23） | `/usr/local/cuda` → `cuda-11.4` |
| cuDNN | 8.6.0.166-1+cuda11.4 | libcudnn8 与 libcudnn8-dev 均已装 |
| TensorRT | 8.5.2.2-1+cuda11.4 | `tensorrt`、`tensorrt-libs`、`libnvinfer-bin` |
| trtexec | TensorRT v8502 | 软链 `/usr/local/bin/trtexec` → `/usr/src/tensorrt/bin/trtexec` |
| nvidia-tensorrt 元包 | 5.1.6-b5 |  |
| Python | 3.8.10（系统） |  |
| GCC | 9.4.0 |  |

未安装 / 未验证：

- `nvidia-jetpack` 元包未装（不影响使用）
- TensorRT Python 绑定未装（`import tensorrt` 报错），**按计划用 C++ 开发，不需要**
- g++/cmake、OpenCV、PyTorch、CUDA samples（deviceQuery）尚未验证

## 3. 电源与性能模式

- 当前模式：`MODE_15W_DESKTOP`（编号 7），`nvpmodel -q` 查询
- 此模式下 4 核在线，其余核心离线
- 风扇：动态控制（`hwmon3_pwm1=0`，空闲时不转）
- 如需最大性能：`sudo nvpmodel -m 0`（MAXN），再 `sudo jetson_clocks` 锁频；先确认供电与散热。编号用 `sudo nvpmodel -p --verbose` 核对
- 恢复默认频率：`sudo jetson_clocks --restore`

## 4. 空闲状态（tegrastats）

| 项目 | 数值 |
| --- | --- |
| CPU 负载 | 0–2%，约 1187 MHz |
| GPU | 0%，318 MHz，功耗 0 mW |
| 内存占用 | 940 / 14887 MB |
| 温度 | CPU 46°C、GPU 46°C、thermal 45°C、Tboard 45°C、Tdiode 47°C、PMIC 50°C、AO 43.5°C |
| 功耗 | CPU 469 mW、SOC 1252 mW、VDDRQ 156 mW、SYS5V 约 2342 mW |

结论：空闲温度余量充足。满载温度尚未测。

## 5. 推理验证

命令：

```bash
trtexec --onnx=/usr/src/tensorrt/data/mnist/mnist.onnx --iterations=2000 --duration=60
```

| 指标 | 结果 |
| --- | --- |
| 结果 | `&&&& PASSED` |
| 吞吐 | 16600 qps |
| GPU 计算时间 | 平均 0.0576 ms |
| Host 延迟 | 平均 0.0637 ms，p99 0.0801 ms |
| Engine 构建 | 16.5 s |
| 层分布 | 全部在 GPU，未用 DLA |
| 告警 | GPU 计算时间波动 5.5%（模型太小，可忽略；锁频可改善） |

说明：CUDA、cuDNN、TensorRT 整条链路正常。mnist 太小，不代表 YOLOv8 的负载，真实性能与温度待实测。

## 6. 网络

| 项目 | 内容 |
| --- | --- |
| 有线口 | eth0，连接名 `eth0-static`（手动配置） |
| IP | 192.168.137.215/24 |
| 网关 | 192.168.137.1（Windows 网络共享网卡） |
| DNS | 223.5.5.5、8.8.8.8 |
| 自动连接 | 是，优先级 100 |
| 默认路由 | `via 192.168.137.1 dev eth0`，metric 100 |
| USB 设备模式 | l4tbr0，192.168.55.1/24（备用通道，metric 32766） |
| 旧连接 | `Wired connection 1` 已删除 |
| 验证 | 重启后 IP 不变；`ping baidu.com` 0% 丢包 |

SSH：

```bash
ssh nvidia@192.168.137.215
```

换电脑或换网段注意：

- Windows 开启网络共享后，网卡会自动是 192.168.137.1，插上即用
- 接到其他网段（如路由器 192.168.1.x）需改回 DHCP 或改成对应网段静态 IP：

  ```bash
  sudo nmcli con mod eth0-static ipv4.method auto ipv4.addresses "" ipv4.gateway "" ipv4.dns ""
  sudo nmcli con up eth0-static
  ```

## 7. 目录与工具

| 路径 | 用途 |
| --- | --- |
| `/data/models` | 模型存放（已创建） |
| `/data/workspace` | 工作目录（已创建） |
| `/usr/src/tensorrt/samples` | 官方 C++ 示例 |
| `/usr/src/tensorrt/data/mnist/mnist.onnx` | 测试模型 |

C++ 编译（典型）：

```bash
g++ main.cpp -o main \
  -I/usr/local/cuda/include -L/usr/local/cuda/lib64 \
  -lnvinfer -lnvonnxparser -lcudart
```

自己编译官方 sample 时曾缺 `cuda_profiler_api.h`，需补装 `cuda-toolkit-11-4`。

## 8. 相机（已到货，待联调）

| 项目 | 内容 |
| --- | --- |
| 型号 | 云天励飞 intellifusion IFC-N2324-FcF 网络摄像机（2019/06，固件 V5.5.60） |
| 供电 | DC 12V 或 PoE（802.3af），最大 9.5 W；现有适配器 12V/2A |
| 镜头 | CS 口 11–40 mm F1.4，1/1.8"，自动光圈，已拧上机身 |
| 接口 | LAN(PoE)、DC 12V 端子、RESET、SD 卡槽、音频、RS485、报警 |
| 取流方式 | RTSP / ONVIF，经网线到 Jetson |

待办：相机通电联网、找 IP 并改到 192.168.137.x 网段、确认 RTSP 地址、调焦。

## 9. 常用验证命令

```bash
cat /etc/nv_tegra_release                 # L4T 版本
dpkg -l | grep -E "cuda-toolkit|tensorrt|libcudnn"
/usr/local/cuda/bin/nvcc --version
trtexec --help | head -n 2
sudo nvpmodel -q
sudo tegrastats                           # 实时温度/功耗/GPU 占用
ip -4 a show eth0; ip route
```

## 10. 后续计划

1. 相机联调，拿到 RTSP 流
2. PC 导出 YOLOv8 ONNX，Jetson 上 `trtexec --fp16` 生成 engine
3. C++ 写取流 → 预处理 → TensorRT 推理 → NMS → 判断逻辑
4. 用真实模型满载压测，记录温度与功耗