# 火花监控项目交接（2026-11-25）

> 本文面向下一位接手模型和现场操作人员。先读现状，再按后面的操作顺序执行，不要重复已经完成的高风险测试。

## 1. 当前状态与系统结构

### 1.1 当前结论

- 火花 C++ 在线服务、网页标定、事件抓拍/录像、飞书通知和 Y1 继电器联动已经部署到 Jetson。
- 已修复“ROI 全部有效但仍返回 `recalibration_required: complete=0`”的后端逻辑错误。
- 修复后 `/api/start_monitoring` 已实际返回 `monitoring_started`，当前服务处于 `MONITORING`。
- 当前 **Y1 联动禁用且线圈断电**。在确认识别稳定前不要启用。
- 飞书已经启用，测试卡片发送成功；真实火花事件卡片仍待 AI 视频重新播放后验证。
- 最近一次正式监控观察窗口约 12 秒，没有事件。这不能作为“AI 火花检测失败”的结论，需要重新播放视频再测。

### 1.2 运行路径

| 项目 | 路径或地址 |
| --- | --- |
| Legion 源码 | `D:\Codex\gen-vision-sentinel` |
| Jetson 源码 | `/data/workspace/gen-vision-sentinel` |
| 生产配置 | `/data/workspace/gen-vision-sentinel/spark/spark_config.json` |
| 生产服务 | `spark/src/bin/spark_monitor_server --config spark/spark_config.json` |
| 生产事件产物 | `/data/workspace/gen-vision-sentinel/spark/recordings/` |
| 当前服务日志 | `/data/workspace/spark-jetson-validation/production-service-calibration-fix-20261125.log` |
| 当前服务 PID 文件 | `/data/workspace/spark-jetson-validation/production-service.pid` |
| Jetson Web | `127.0.0.1:8081` |
| Legion 浏览器 | `http://127.0.0.1:18081` |

### 1.3 视频与标定

- 当前相机为 `1920x1080`，实测约 `25 FPS`。
- 当前保存的 ROI 为：

```text
contact_roi   = [956, 388, 1114, 437]
falling_roi   = [930, 437, 1180, 850]
reference_roi = [700, 160, 900, 240]
```

- 红色接触框位于中间碳刷接触线，橙色下落框覆盖下方路径，蓝色参照框位于左侧静止金属面。
- 摄像头移动、对焦变化或视频源改变后，必须重新抓帧核对，不能直接沿用以上坐标。
- 当前标定已确认完成，但 AI 视频已经停止或离开火花片段时，需要重新播放再做事件测试。

### 1.4 继电器与飞书

- 继电器：`192.168.0.7:8234`，火花只管理 Y1，不复位水流项目的 Y2-Y8。
- Y1 行为：确认 `event_triggered` 后通电吸合并锁存；网页手动复位、进入标定、视频结束或正常退出时断电。
- `relay_connected=true` 只表示 TCP 已连接，`relay_energized=true` 表示已经下发通电指令，不等于读取了触点状态。
- 飞书凭证从既有配置或 `.env` 读取，本文不记录任何密钥。`web_monitor_url=auto` 只是跳转地址，不是凭证。

## 2. 已完成的验证

### 2.1 算法与构建

- 既有 Jetson 报告中的 Python/C++ 对拍已通过：样本图、真实正常录像、AI 参考片和标定配置兼容性均使用默认 `1e-9` 容差。
- AI 参考片历史结果为 2 个 `heavy / blue_white` 事件，约 `3875ms` 和 `4708ms`。AI 素材不是现场真实样本，不能代替现场误报/漏报验收。
- 当前生产源码已在 Jetson ARM 环境使用 `-Wall -Wextra -pedantic` 编译通过。

### 2.2 继电器验证

- 回环假继电器集成测试通过：

```text
EVENT_COUNT=1
MONITOR_RELAY_TEST_OK
RELAY_PACKETS_OK packets=4 on=1 off=3 channels=Y1
CALIBRATION_FIX_REGRESSION_OK
```

- 该回归专门使用 `calibration.complete=false`，覆盖本次“无法确认标定”的修复。
- 假继电器确认只出现 Y1 指令，没有 Y2-Y8 指令。
- 真实 Y1 已做过两次短促通断电测试，第二次约 5 秒，现场人员明确听到动作声；测试后读取线圈为 `OFF`。
- 后续无需反复做真实继电器动作。除非项目主当次明确要求，不要再次通电。

### 2.3 飞书与标定

- 飞书测试卡片日志为：`[FeishuClient-C++] 告警卡片发送成功: 【测试】火花监测飞书联动`。
- 当前配置已启用飞书，但真实事件卡片尚未验证。
- 标定修复前，三个有效 ROI 仍返回 `recalibration_required: complete=0`。
- 修复后允许在分辨率一致且三个 ROI 有效时确认标定，已实测 `monitoring_started`。

### 2.4 当前不能宣称通过的项目

- 不能宣称 AI 视频端到端事件测试已通过，当前只验证到“正式监控启动成功”，需要重新播放视频。
- 不能宣称现场长时间无误报、无漏报。
- 不能宣称开机自启、摄像头断流恢复或手机触屏实机验收已完成。
- 不能宣称当前 P2P 网络能够稳定承载原始 1080p MJPEG 视频。

## 3. 剩余问题与注意事项

### 3.1 网络与浏览器访问

- Jetson 通过 USB 手机网络出网，现场日志出现 `HardNAT`、`BehaviorPortChanged` 和 QUIC 超时。
- 当前 Visitor 配置是 **XTCP P2P 优先，STCP 公网中转兜底**，不是纯中转，也不是纯 P2P。
- P2P 打洞成功不代表长期视频稳定。原始预览约 20 FPS、1080p MJPEG，链路不稳定时 SSH 长连接会断开。
- 曾使用临时 STCP 配置做对照，但测试后已恢复 `visitor.toml` 的 P2P 优先配置。
- `D:\Codex\frp\controller\visitor-stcp-test.toml` 是临时测试产物，不是当前活动配置，不要误用或上传。

### 3.2 本地转发实现

- 网页访问链路为：Legion 浏览器 `18081` → SSH 本地转发 → 本地 Visitor `60022` → FRP → Jetson 便携 SSH → Jetson `8081`。
- 摄像头 `554` 没有单独映射，网页视频由 Jetson MJPEG 服务提供。
- 便携 SSH 重启后 `AllowTcpForwarding` 会恢复为 `no`，需要在获得项目主授权后重新设置为 `yes` 并重载便携 SSH。
- 不修改系统 `/usr/sbin/sshd`，也不修改公网 FRP。

### 3.3 Go 控制端扩展

- 原 `agent-controller.exe` 未替换。
- 为端口转发新增了独立程序：`D:\Codex\frp\controller\linux_go\agent-controller-forward.exe`。
- 对应源码为 `D:\Codex\frp\controller\linux_go\src\forward.go`，并在 `src/main.go` 注册了 `forward` 命令。
- 这些控制端文件位于本仓库之外，不属于本次 GitHub 功能提交。

### 3.4 生产运行缺口

- Jetson 重启后火花服务不会自动启动，需要补正式的开机自启动方案。
- RTSP 取流中断后的自动重连和离线状态更新仍需完善，不能把进程存活当作视频正常。
- 摄像头 OSD 时间仍显示 `1970年01月05日`，需要在校验录像时间前处理；服务器事件时间使用 Jetson 系统时间。
- 网页 API 使用 GET 执行状态修改且没有鉴权，只应在受控网络中使用，不要直接暴露公网。
- 触屏标定已改用 Pointer Events，但还需要手机浏览器实测。
- 配置和录像中的验证材料可能含敏感信息，不要提交 `.env`、`spark_config.json` 或现场凭证。

## 4. 下一模型的操作顺序

### 4.1 先读控制规范

必须阅读：

```text
D:\Codex\frp\controller\linux_go\AGENT_GUIDE_LINUX.md
```

- 远端 shell 默认只使用 Go 控制端的 `type` 同屏执行。
- 不使用裸 `ssh`、`scp`、静默 `exec` 或旧 `remote_exec.bat`。
- 不修改公网 FRP，不执行 `stop`、`remove`，不复位 Y2-Y8。
- 不打印 RTSP 地址、飞书密钥或 `agent_key`。
- 真实继电器动作必须获得项目主当次明确许可。

### 4.2 核对服务状态

```powershell
$controller = 'D:\Codex\frp\controller\linux_go\agent-controller.exe'
& $controller type 'cd /data/workspace/gen-vision-sentinel; ps -p "$(cat /data/workspace/spark-jetson-validation/production-service.pid)" -o pid,stat,etime,cmd; python3 -c ''import json,urllib.request; s=json.load(urllib.request.urlopen("http://127.0.0.1:8081/api/status",timeout=5)); print(s["mode"],s["source_online"],s["frame_width"],s["frame_height"],s["relay_trip_enabled"],s["relay_energized"])'''
```

若进程不存在，先确认配置没有变化，再启动：

```bash
cd /data/workspace/gen-vision-sentinel
nohup spark/src/bin/spark_monitor_server --config spark/spark_config.json \
  > /data/workspace/spark-jetson-validation/production-service-calibration-fix-20261125.log 2>&1 &
```

将新 PID 写入 `/data/workspace/spark-jetson-validation/production-service.pid`。不要结束名称相似但归属不明的进程。

### 4.3 恢复 Legion 浏览器

```powershell
& 'D:\Codex\frp\controller\linux_go\agent-controller-forward.exe' forward 18081 127.0.0.1 8081
```

保持该进程运行，浏览器打开 `http://127.0.0.1:18081`。若提示转发被禁止，先检查便携 SSH 的 `AllowTcpForwarding`；修改和重载前需要项目主授权。

### 4.4 立即进行 AI 视频事件测试

1. 保持 **Y1 联动禁用**，飞书保持启用。
2. 抓一帧，确认镜头没有移动、三个 ROI 仍覆盖正确位置。
3. 让项目主全屏循环播放 AI 火花视频。
4. 确认 `/api/status` 为 `MONITORING`，不要在视频结束后才启动检测。
5. 观察至少一个完整播放周期，记录 `events`、`recording_error`、抓拍、录像和飞书日志。
6. 功能目标是至少产生一个事件并生成可读取的抓拍/录像。历史离线结果为 2 个 `heavy / blue_white`，但摄像头拍摄屏幕的结果不应机械要求逐帧一致。
7. 如果没有事件，先读取实时指标和当前画面，不要盲目提高阈值或启用 Y1。

状态查询示例：

```bash
python3 -c 'import json,urllib.request; s=json.load(urllib.request.urlopen("http://127.0.0.1:8081/api/status",timeout=5)); print(json.dumps({"mode":s["mode"],"events":s["events"],"recording_error":s["recording_error"],"relay":s["relay_energized"],"metrics":s["metrics"]},ensure_ascii=False))'
```

### 4.5 最后才做真实 Y1 联动

- 只有在 AI 事件、抓拍、录像和飞书均正确后，才提出真实 Y1 联动测试。
- 必须获得项目主当次明确许可，并让其观察 Y1 指示灯和动作声音。
- 测试后读取线圈状态并确认 `OFF`，随后再次禁用联动，避免调参期间误动作。

### 4.6 收尾与后续工程任务

- 真实正常画面连续运行，记录误报、漏报、帧率和录像完整性。
- 增加 Jetson 开机自启动和 RTSP 断流重连。
- 根据实际带宽决定是否把网页预览降为 5 FPS；检测器仍保持 25 FPS。该降帧方案曾提出但未部署，不要重复误报为已上线。
- 实测手机触屏标定，并处理摄像头 OSD 时间。
- Git 提交只包含源码、测试和文档，不包含 `.env`、生产配置、录像、日志、AI 素材或临时验证目录。
