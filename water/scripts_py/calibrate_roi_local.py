#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
发电机冷却水管口标定工具 (Windows 本地独立运行版)
无需开发板屏幕，直接在您的 Windows 电脑桌面上弹出交互窗口拉框标定。

用法：
  # 方式 1：标定本地视频
  python water/scripts_py/calibrate_roi_local.py water/videos/water.mp4

  # 方式 2：直接标定网络摄像头 RTSP
  python water/scripts_py/calibrate_roi_local.py "rtsp://username:password@camera-host:554/path"
"""

import sys
import os
import json
import cv2

# 默认 5 根管道基准
DEFAULT_PIPES = [
    {"id": 1, "name": "左侧主粗管(大流量)", "roi": [390, 580, 550, 750], "threshold": 4.0},
    {"id": 2, "name": "中间细黄管(直流水)", "roi": [550, 500, 600, 600], "threshold": 3.5},
    {"id": 3, "name": "中偏右银管(散流水)", "roi": [650, 530, 730, 600], "threshold": 3.5},
    {"id": 4, "name": "右侧弯管口(直流水)", "roi": [720, 640, 780, 700], "threshold": 3.5},
    {"id": 5, "name": "最右顺壁管(贴壁细流)", "roi": [790, 600, 830, 700], "threshold": 2.5}
]

drawing = False
pt1 = (0, 0)
pt2 = (0, 0)
temp_roi = None

def on_mouse(event, x, y, flags, param):
    global drawing, pt1, pt2, temp_roi
    if event == cv2.EVENT_LBUTTONDOWN:
        drawing = True
        pt1 = (x, y)
        pt2 = (x, y)
        temp_roi = None
    elif event == cv2.EVENT_MOUSEMOVE:
        if drawing:
            pt2 = (x, y)
            x1, y1 = min(pt1[0], pt2[0]), min(pt1[1], pt2[1])
            w, h = abs(pt1[0] - pt2[0]), abs(pt1[1] - pt2[1])
            temp_roi = (x1, y1, w, h)
    elif event == cv2.EVENT_LBUTTONUP:
        drawing = False
        pt2 = (x, y)
        x1, y1 = min(pt1[0], pt2[0]), min(pt1[1], pt2[1])
        w, h = abs(pt1[0] - pt2[0]), abs(pt1[1] - pt2[1])
        if w > 10 and h > 10:
            temp_roi = (x1, y1, w, h)
            print(f"\n[已画框] 区域: [{x1}, {y1}, {x1+w}, {y1+h}] (宽{w}x高{h})")
            print("  👉 请按键盘数字键 [1 ~ 5] 绑定给对应管道；或按其他键重新画框")

def main():
    global temp_roi
    print("=" * 60)
    print("  冷却水管口标定工具 (Windows 本地桌面弹窗版)")
    print("=" * 60)
    print("  操作说明：")
    print("    * [空格键]    : 暂停 / 播放 (暂停时可用鼠标慢慢画框)")
    print("    * [鼠标左键]  : 在画面上拖拽拉框")
    print("    * [数字 1 ~ 5]: 将刚画的框分配给对应管道 Pipe 1 ~ 5")
    print("    * [按键 S]    : 将当前 5 个坐标保存至 water/water_config.json")
    print("    * [按键 Q/ESC]: 保存标定预览图并退出")
    print("=" * 60)

    # 寻找配置文件
    config_file = "water/water_config.json"
    if not os.path.exists(config_file):
        config_file = "../water_config.json"
    if not os.path.exists(config_file):
        config_file = "water_config.json"

    pipes = DEFAULT_PIPES
    if os.path.exists(config_file):
        try:
            with open(config_file, "r", encoding="utf-8") as f:
                cfg = json.load(f)
                if "pipes" in cfg:
                    pipes = cfg["pipes"]
                    print(f"[配置] 成功从 {config_file} 加载已有的管道坐标。")
        except Exception as e:
            print(f"[警告] 读取配置文件失败: {e}，使用默认参数。")

    # 视频输入源
    src = sys.argv[1] if len(sys.argv) > 1 else "water/videos/water.mp4"
    if not os.path.exists(src) and not src.startswith("rtsp://") and not src.startswith("http://"):
        src = "../videos/water.mp4"

    is_live = src.startswith("rtsp://") or src.startswith("http://")
    print(f"[视频源] {'网络摄像头实时流' if is_live else '本地视频文件'}: {src}")

    cap = cv2.VideoCapture(src)
    if not cap.isOpened():
        print(f"❌ 无法打开视频源: {src}")
        return

    win_name = "Water Pipe ROI Calibration (Windows Local)"
    cv2.namedWindow(win_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win_name, 540, 960)
    cv2.setMouseCallback(win_name, on_mouse)

    paused = True  # 默认初始暂停，方便直接画框
    ret, frame = cap.read()
    if not ret or frame is None:
        print("❌ 读取视频首帧失败！")
        return

    while True:
        if not paused:
            ret, frame = cap.read()
            if not ret or frame is None:
                if not is_live:
                    cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                    ret, frame = cap.read()
                else:
                    cv2.waitKey(30)
                    continue
        elif is_live:
            # 清空缓存
            cap.grab()

        display = frame.copy()

        # 绘制半透明顶部提示
        overlay = display.copy()
        cv2.rectangle(overlay, (0, 0), (display.shape[1], 100), (15, 15, 15), -1)
        cv2.addWeighted(overlay, 0.75, display, 0.25, 0, display)

        cv2.putText(display, "PIPE ROI CALIBRATOR (WINDOWS)", (20, 32),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2, cv2.LINE_AA)
        status_txt = "PAUSED (DRAG MOUSE TO DRAW)" if paused else "PLAYING VIDEO"
        cv2.putText(display, f"STATUS: {status_txt}", (20, 62),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 220, 255), 2, cv2.LINE_AA)
        cv2.putText(display, "KEYS: SPACE=Pause | 1-5=Bind Pipe | S=Save Config | Q=Quit", (20, 88),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)

        # 绘制 5 根管口框
        colors = [(0, 230, 0), (255, 180, 0), (0, 200, 255), (200, 100, 255), (100, 255, 200)]
        for i, p in enumerate(pipes):
            x1, y1, x2, y2 = p["roi"]
            col = colors[i % len(colors)]
            cv2.rectangle(display, (x1, y1), (x2, y2), col, 2)
            lbl = f"Pipe {p['id']}: [{x1},{y1},{x2},{y2}]"
            cv2.rectangle(display, (x1, max(110, y1 - 22)), (x1 + 190, max(130, y1)), col, -1)
            cv2.putText(display, lbl, (x1 + 4, max(124, y1 - 6)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0), 1, cv2.LINE_AA)

        # 绘制正在拉的框
        if temp_roi is not None:
            rx, ry, rw, rh = temp_roi
            cv2.rectangle(display, (rx, ry), (rx + rw, ry + rh), (0, 255, 255), 2)
            cv2.putText(display, f"New Box [{rx},{ry},{rx+rw},{ry+rh}] -> Press 1-5", (rx, max(120, ry - 8)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 255), 2, cv2.LINE_AA)

        cv2.imshow(win_name, display)
        key = cv2.waitKey(50 if paused else 30) & 0xFF

        if key in [ord('q'), ord('Q'), 27]:
            out_img = "water/images/water_calibration_preview.jpg"
            os.makedirs("water/images", exist_ok=True)
            cv2.imwrite(out_img, display)
            print(f"\n[退出] 最新标定画面已保存至: {out_img}")
            break
        elif key == 32:  # 空格键
            paused = not paused
            print(f"[切换] {'已暂停视频' if paused else '继续播放视频'}")
        elif ord('1') <= key <= ord('5'):
            target_id = key - ord('0')
            if temp_roi is not None:
                rx, ry, rw, rh = temp_roi
                for p in pipes:
                    if p["id"] == target_id:
                        p["roi"] = [rx, ry, rx + rw, ry + rh]
                        print(f"🎯 成功将 Pipe {target_id} 更新为新坐标: {p['roi']}")
                        print("👉 请按 [S] 键保存到配置文件！")
                        temp_roi = None
                        break
            else:
                print(f"[提示] 请先用鼠标拉框，再按数字键 {target_id} 绑定！")
        elif key in [ord('s'), ord('S')]:
            cfg_data = {
                "device_name": "1号发电机机组 - 冷却水智能视觉防护系统",
                "web_port": 8080,
                "alarm_duration_sec": 2.0,
                "recovery_duration_sec": 2.0,
                "pipes": pipes
            }
            with open(config_file, "w", encoding="utf-8") as f:
                json.dump(cfg_data, f, indent=2, ensure_ascii=False)
            print(f"\n✅ 5 根管口坐标已成功保存到: {config_file}")

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
