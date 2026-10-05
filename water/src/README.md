# 冷却水系统 C++ 生产核心源码目录 (`water/src`)

本目录存放发电机冷却水监控系统的正式生产级原生 C++ 核心源码与服务入口。测试与标定工具已全部独立移入 `test/` 子目录中。

---

## 目录分层结构

```text
water/src/
├── 📁 test/                     # 【测试与标定专用目录】
│   ├── 📄 water_flow_analyzer.cpp # 纯 C++ 实时动能看板、抗抖微调、Web推流与飞书联动一体机 (已全面打通)
│   ├── 📄 calibrate_roi.cpp       # 纯 C++ 鼠标交互画框标定工具
│   ├── 📄 Makefile                # 测试工具专属编译脚本 (链接 GStreamer, OpenCV Core, libcurl)
│   └── 📑 README.md               # 测试目录使用说明
├── 📄 water_monitor_server.cpp  # 【生产服务主入口】RTSP硬解码 + Web大屏推流 + 告警联动
├── 📄 water_flow_detector.h/.cpp# 【核心检测引擎】5根管口动能计算 + 三级防误报状态机
├── 📄 feishu_client.h/.cpp      # 【飞书客户端】libcurl 原生异步秒发组件 (开机后台预取Token常驻内存，零微秒阻塞)
├── 📄 test_feishu_standalone.cpp# 【飞书独立测试】纯 C++ 原生飞书联调验证入口
├── 📄 Makefile                  # 生产服务一键编译脚本
└── 📑 README.md                 # 本说明文档
```

---

## 正式服务编译与运行指南 (Jetson AGX Xavier)

```bash
# 1. 编译正式监控推流服务
cd /data/workspace/water/src
make clean && make -j4

# 2. 启动服务 (自动广播 8080 Web大屏)
./water_monitor_server
```
