# 现场报警与通信联动模块 (`alarm_py`)

> [!NOTE]
> **架构定位说明**：
> 按照发电机机房生产开发规范，**开发板端生产运行已 100% 切换为纯 C++ 原生驱动**（参见 `water/src/feishu_client.h / .cpp`，基于 `libcurl` 异步独立线程，开机自动预取 Token 常驻内存，零 Python 依赖）。
> 本目录归档系统对外联动的**双支柱告警输出驱动与推送脚本**，主要用于 **Windows 本地离线测试、协议报文仿真及继电器接口验证**。

---

## 文件列表与核心功能

| 文件 | 通信协议 / 类型 | 核心作用与业务职责 |
| :--- | :--- | :--- |
| **`feishu_alarm.py`** | HTTP/HTTPS (REST API) | **飞书群机器人核心报警客户端**：封装水流断流紧急报警（红底）、短时波动预警（橙底）、水流恢复通知（绿底）及碳刷打火告警，自带 HMAC-SHA256 签名校验与大屏跳转按钮。 |
| **`feishu_config.json`** | 配置文件 | **现场免改代码配置文件**：外置 Webhook URL、安全密钥、设备名及大屏查看地址，修改即刻生效。 |
| **`modbus_relay.py`** | Modbus-TCP (Port 502) | **中盛科技 8 路以太网继电器 (ZS-DIO-R-10A-8) 驱动**：支持原生 Socket 发送标准 Modbus-TCP 报文，对 Y1 ~ Y8 无源干接点进行单线圈控制、脉冲点动（闭合 $T$ 秒后自动释放复位）及一键全关。 |
| **`test_feishu_push.py`** | 测试脚本 | **早期基础推送测试代码**（已升级沉淀至 `feishu_alarm.py`）。 |

---

## 8 路继电器输出端子 (Y1 ~ Y8) 映射规范

| 继电器编号 | 端子标识 | 对应告警业务 | 触发条件 | 现场 PLC / 柜内功能 |
| :---: | :---: | :--- | :--- | :--- |
| **Y1** | `NO1 / COM1` | **碳刷火花打火** | 捕捉到碳刷接触面持续打火 (>200ms) | 接入碳刷保护回路 / 声光警报 |
| **Y2** | `NO2 / COM2` | **冷却水系统总告警** | 任意 1 根或多根水管发生断流 | 接入冷却水总联锁 / 启动备用水泵 |
| **Y3** | `NO3 / COM3` | **Pipe 1 异常** | 1 号主粗管断水 / 堵塞 | 定位 1 号管道故障 |
| **Y4** | `NO4 / COM4` | **Pipe 2 异常** | 2 号细黄管断水 / 堵塞 | 定位 2 号管道故障 |
| **Y5** | `NO5 / COM5` | **Pipe 3 异常** | 3 号中银管断水 / 堵塞 | 定位 3 号管道故障 |
| **Y6** | `NO6 / COM6` | **Pipe 4 异常** | 4 号弯管口断水 / 堵塞 | 定位 4 号管道故障 |
| **Y7** | `NO7 / COM7` | **Pipe 5 异常** | 5 号顺壁管断水 / 堵塞 | 定位 5 号管道故障 |
| **Y8** | `NO8 / COM8` | **Pipe 6 异常** | 6 号管道断水 / 堵塞 | 定位 6 号管道故障 |

---

## 快速使用与联调测试指南

### 1. 飞书机器人报警推送 (`feishu_alarm.py`)
在 `feishu_config.json` 填入真实 Webhook 后，即可通过命令行或代码直接触发：

#### 命令行自测指令：
```bash
# 1. 模拟触发 2 号水管断流紧急告警 (红底高危卡片)
python feishu_alarm.py --test-water

# 2. 模拟触发水流短时偏低预警 (橙底预警卡片)
python feishu_alarm.py --test-warn

# 3. 模拟触发 2 号水管恢复供水通知 (绿底正常卡片)
python feishu_alarm.py --test-recovery

# 4. 模拟触发发电机碳刷打火紧急告警 (红底高危卡片)
python feishu_alarm.py --test-spark
```

#### 在水流检测代码中调用：
```python
from feishu_alarm import FeishuAlarmClient

client = FeishuAlarmClient()

# 检测到 Pipe 2 持续断流 2.4 秒：
client.notify_water_flow_alarm(
    pipe_id=2,
    pipe_name="细黄管(直流水)",
    current_energy=0.0,
    threshold=3.5,
    duration_sec=2.4
)
```

---

### 2. 硬件继电器测试 (`modbus_relay.py`)
```bash
# 默认连接 192.168.1.200:502，触发 Y2 (冷却水总警报) 闭合 1 秒后自动复位
python modbus_relay.py 192.168.1.200
```

