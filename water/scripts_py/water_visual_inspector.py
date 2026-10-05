#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
发电机冷却水 5 根管道可视化检测与交互式坐标标定工具
功能：
  1. 实时计算并展示 5 根水管的动能数值与性能看板 (纯本地显示，不发飞书)
  2. 支持鼠标在画面上拖拽自由画框与坐标标定微调
  3. 按键一键保存坐标至 water_config.json
  4. 支持按键模拟注入断水演练 (看框由绿变橙变红)
"""

import cv2
import json
import time
import os
import sys
import numpy as np

# 路径定位
CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
WATER_DIR = os.path.dirname(CURRENT_DIR)
CONFIG_PATH = os.path.join(WATER_DIR, "water_config.json")
VIDEO_PATH = os.path.join(WATER_DIR, "videos", "water.mp4")
OUTPUT_IMAGE = os.path.join(WATER_DIR, "images", "water_inspection_snapshot.jpg")

# 全局鼠标交互状态
g_drawing = False
g_ix, g_iy = -1, -1
g_cx, g_cy = -1, -1
g_temp_box = None

def mouse_callback(event, x, y, flags, param):
    global g_drawing, g_ix, g_iy, g_cx, g_cy, g_temp_box
    if event == cv2.EVENT_LBUTTONDOWN:
        g_drawing = True
        g_ix, g_iy = x, y
        g_cx, g_cy = x, y
        g_temp_box = None
    elif event == cv2.EVENT_MOUSEMOVE:
        if g_drawing:
            g_cx, g_cy = x, y
            x1 = min(g_ix, g_cx)
            y1 = min(g_iy, g_cy)
            x2 = max(g_ix, g_cx)
            y2 = max(g_iy, g_cy)
            g_temp_box = [x1, y1, x2, y2]
    elif event == cv2.EVENT_LBUTTONUP:
        g_drawing = False
        g_cx, g_cy = x, y
        x1 = min(g_ix, g_cx)
        y1 = min(g_iy, g_cy)
        x2 = max(g_ix, g_cx)
        y2 = max(g_iy, g_cy)
        if (x2 - x1) > 10 and (y2 - y1) > 10:
            g_temp_box = [x1, y1, x2, y2]
            print(f"\n[标定提示] 鼠标已选定新框区域: [{x1}, {y1}, {x2}, {y2}]")
            print("  👉 请按下数字键 [1 ~ 5] 将此框分配给对应管道；按 [C] 取消")

def load_config():
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)

def save_config(config_data):
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(config_data, f, indent=2, ensure_ascii=False)
    print(f"\n✅ 成功将最新管口坐标保存至: {CONFIG_PATH}")

def main():
    if not os.path.exists(VIDEO_PATH):
        print(f"错误: 未找到测试视频: {VIDEO_PATH}")
        return

    config = load_config()
    pipes = config.get("pipes", [])

    print("==========================================================")
    print("  发电机冷却水 5 根管道可视化检测与交互式标定工具")
    print("==========================================================")
    print("  [快捷键说明]：")
    print("    * [空格键]   : 暂停 / 继续播放 (暂停时方便精细画框)")
    print("    * [鼠标拖拽] : 在水流处拉出一个矩形框")
    print("    * [数字 1~5] : 将刚画的框分配给对应管道 (Pipe 1 ~ 5)")
    print("    * [按键 S]   : 将当前所有坐标保存到 water_config.json")
    print("    * [按键 C]   : 开启/关闭【2号管模拟断水演练】(看框变红)")
    print("    * [按键 Q/ESC]: 保存效果图并退出")
    print("==========================================================")

    cap = cv2.VideoCapture(VIDEO_PATH)
    if not cap.isOpened():
        print(f"无法打开视频: {VIDEO_PATH}")
        return

    window_name = "Water Flow Visual Inspector & Calibration (Safe Mode - No Feishu Push)"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 540, 960) # 适中显示比例
    cv2.setMouseCallback(window_name, mouse_callback)

    # 管道运行态数据初始化
    pipe_energies = {p["id"]: p.get("threshold", 3.5) * 2.0 for p in pipes}
    pipe_low_start = {p["id"]: 0.0 for p in pipes}
    pipe_states = {p["id"]: "NORMAL" for p in pipes}
    simulated_cutoff_pipe2 = False
    paused = False

    prev_gray = None
    last_frame = None
    frame_count = 0
    fps = 30.0
    t_start = time.time()

    while True:
        if not paused:
            ret, frame = cap.read()
            if not ret or frame is None:
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            last_frame = frame.copy()
        else:
            if last_frame is None:
                continue
            frame = last_frame.copy()

        now = time.time()
        elapsed = now - t_start

        # 1. 动能计算 (仅在非暂停时计算)
        if not paused:
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            blurred = cv2.GaussianBlur(gray, (5, 5), 0)

            if prev_gray is not None and prev_gray.shape == blurred.shape:
                diff = cv2.absdiff(blurred, prev_gray)
                _, thresh = cv2.threshold(diff, 12, 255, cv2.THRESH_BINARY)

                for p in pipes:
                    pid = p["id"]
                    if not p.get("enabled", True):
                        continue

                    if pid == 2 and simulated_cutoff_pipe2:
                        raw_e = 0.0
                    else:
                        x1, y1, x2, y2 = p["roi"]
                        roi_diff = thresh[y1:y2, x1:x2]
                        if roi_diff.size > 0:
                            active = cv2.countNonZero(roi_diff)
                            raw_e = (active / float(roi_diff.size)) * 100.0
                        else:
                            raw_e = 0.0

                    # 平滑滤波
                    pipe_energies[pid] = 0.7 * raw_e + 0.3 * pipe_energies[pid]
                    th = p.get("threshold", 3.5)

                    # 状态机判断
                    if pipe_energies[pid] < th:
                        if pipe_low_start[pid] == 0.0:
                            pipe_low_start[pid] = elapsed
                        dur = elapsed - pipe_low_start[pid]
                        if dur >= 2.0:
                            pipe_states[pid] = "ALARM"
                        elif dur >= 0.5:
                            pipe_states[pid] = "WARN"
                    else:
                        pipe_low_start[pid] = 0.0
                        pipe_states[pid] = "NORMAL"

            prev_gray = blurred

        # 2. 绘制画面与 OSD 看板
        display = frame.copy()
        h, w = display.shape[:2]

        # 顶部深色半透明工业看板
        header_h = 130
        overlay = display[0:header_h, 0:w].copy()
        dark_bg = np.zeros_like(overlay)
        cv2.addWeighted(dark_bg, 0.75, overlay, 0.25, 0, display[0:header_h, 0:w])

        # 标题与状态 (使用标准 ASCII 字体杜绝问号)
        cv2.putText(display, "WATER FLOW MONITOR - 5 PIPES INDEPENDENT", (20, 35),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.85, (255, 255, 255), 2, cv2.LINE_AA)

        sim_text = "[SIMULATING CUTOFF: Pipe 2]" if simulated_cutoff_pipe2 else "[ALL PIPES NORMAL]"
        pause_text = " - PAUSED (DRAG MOUSE TO DRAW BOX)" if paused else " - REALTIME PLAYING"
        state_color = (0, 140, 255) if simulated_cutoff_pipe2 else (0, 255, 120)
        cv2.putText(display, f"STATUS: {sim_text}{pause_text}", (20, 70),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, state_color, 2, cv2.LINE_AA)

        cv2.putText(display, "KEYS: SPACE=Pause/Play | Drag Mouse=Box | 1~5=Bind Pipe | S=Save | C=Cutoff | Q=Quit", (20, 105),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.52, (200, 200, 200), 1, cv2.LINE_AA)


        # 绘制 5 根管口的框
        for p in pipes:
            if not p.get("enabled", True):
                continue
            pid = p["id"]
            x1, y1, x2, y2 = p["roi"]
            st = pipe_states[pid]
            e = pipe_energies[pid]
            th = p.get("threshold", 3.5)

            if st == "ALARM":
                color = (0, 0, 255) # 猩红
                thick = 4
                tag = "ALARM!"
            elif st == "WARN":
                color = (0, 165, 255) # 橙黄
                thick = 2
                tag = "WARN"
            else:
                color = (0, 240, 0) # 绿色
                thick = 2
                tag = "OK"

            cv2.rectangle(display, (x1, y1), (x2, y2), color, thick)

            # 标签背景与文字
            lbl = f"Pipe {pid} [{tag}] E:{e:.1f}/{th:.1f}"
            t_size = cv2.getTextSize(lbl, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 2)[0]
            ty = max(header_h + 20, y1 - 8)
            cv2.rectangle(display, (x1, ty - t_size[1] - 4), (x1 + t_size[0] + 8, ty + 4), color, -1)
            cv2.putText(display, lbl, (x1 + 4, ty), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 2, cv2.LINE_AA)

        # 如果正在鼠标拖拽新框，画出黄色虚线/细线框
        if g_temp_box:
            tx1, ty1, tx2, ty2 = g_temp_box
            cv2.rectangle(display, (tx1, ty1), (tx2, ty2), (0, 255, 255), 2)
            cv2.putText(display, f"新建框: [{tx1},{ty1},{tx2},{ty2}] 请按 1~5 键绑定", (tx1, max(header_h + 20, ty1 - 10)),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2, cv2.LINE_AA)

        cv2.imshow(window_name, display)

        # 键盘监听
        key = cv2.waitKey(30 if not paused else 50) & 0xFF
        if key in [ord('q'), ord('Q'), 27]: # Q 或 ESC
            # 退出前保存一张当前带框的高清效果图
            cv2.imwrite(OUTPUT_IMAGE, display)
            print(f"\n[退出保存] 已将当前带框与看板的效果图保存至: {OUTPUT_IMAGE}")
            break
        elif key == 32: # 空格键
            paused = not paused
            print(f"[状态切换] {'已暂停视频 (请使用鼠标在管口处拖拽拉框)' if paused else '继续播放视频'}")
        elif key in [ord('c'), ord('C')]:
            simulated_cutoff_pipe2 = not simulated_cutoff_pipe2
            print(f"[演练模式] 2号水管模拟断水已切换为: {'【断水注入 (观察框变红)】' if simulated_cutoff_pipe2 else '【恢复正常流水】'}")
        elif key in [ord('s'), ord('S')]:
            save_config(config)
        elif key in [ord('1'), ord('2'), ord('3'), ord('4'), ord('5')]:
            target_id = key - ord('0')
            if g_temp_box:
                for p in pipes:
                    if p["id"] == target_id:
                        p["roi"] = g_temp_box
                        print(f"🎯 成功将管口 Pipe {target_id} ({p['name']}) 坐标更新为: {g_temp_box}")
                        print("👉 按 [S] 键即可保存到配置文件！")
                        g_temp_box = None
                        break
            else:
                print(f"[提示] 请先用鼠标在画面上拖拽拉出一个框，然后再按数字键 {target_id} 绑定！")

    cap.release()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
