# 系统常用运维工具与离线包 (`tools`)

本目录存放专为 NVIDIA Jetson AGX Xavier (aarch64 / ARM64 架构, Ubuntu 20.04 LTS) 准备的工业离线环境工具包。

---

## 工具列表

| 文件名 | 架构 / 适用平台 | 说明与用途 |
| :--- | :--- | :--- |
| **`tmux_arm64.deb`** | `arm64` (aarch64) / Ubuntu 20.04 | **工业现场离线终端复用工具包**：发电机房没有外网互联网，该离线包可直接通过 `dpkg` 命令安装 `tmux`，支持后台进程保活、防止 SSH 意外断开导致监控服务退出。 |
| **`win_proxy.py`** | Windows PC (Python 3.x) | **工位 USB 调试透明过桥代理**：在开发板未插外网网线时，在 Windows 端提供 `10811` 端口 HTTP CONNECT 透明转发，帮助板端 C++ 访问公网飞书服务器 (`open.feishu.cn`)。 |
| **`../一键启动电脑过桥代理(飞书上网用).bat`** | Windows 批处理 | **可见控制前台批处理**：双击即可启动过桥代理（弹出显式绿色 CMD 窗口），不用时点红叉彻底退出，杜绝隐蔽后台。 |

---

## 离线安装与使用指南

### 1. 离线安装命令 (在 Jetson 板端执行)
```bash
sudo dpkg -i tmux_arm64.deb
```

### 2. 常用 tmux 运维命令
* **创建/启动持久化会话**：
  ```bash
  tmux new -s work
  ```
* **进入正在运行的会话（与当前后台同屏）**：
  ```bash
  tmux a -t work
  ```
* **挂起会话（后台继续运行）**：
  快捷键：按下 `Ctrl + B`，然后按 `D`。
* **查看所有活动会话**：
  ```bash
  tmux ls
  ```
