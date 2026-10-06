import socket
import time
import sys

def crc16(data: bytes) -> bytes:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return data + bytes([crc & 0xFF, (crc >> 8) & 0xFF])

def control_channel(s, channel, state):
    # channel: 1~8 -> register 0~7
    reg = channel - 1
    val = 0xFF00 if state else 0x0000
    cmd = crc16(bytes([0x01, 0x05, (reg >> 8) & 0xFF, reg & 0xFF, (val >> 8) & 0xFF, val & 0xFF]))
    s.sendall(cmd)
    resp = s.recv(1024)
    return resp

def main():
    names = [
        "Y1: 碳刷火花打火告警",
        "Y2: 冷却水系统总告警",
        "Y3: Pipe 1 (主粗管)",
        "Y4: Pipe 2 (细黄管)",
        "Y5: Pipe 3 (中银管)",
        "Y6: Pipe 4 (弯管口)",
        "Y7: Pipe 5 (顺壁流)",
        "Y8: Pipe 6 (预留管)",
    ]

    print("==================================================")
    print("  中盛科技 8路继电器 (ZS-DO-R-10A-8) 全通道轮巡测试")
    print("==================================================")
    
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(('192.168.0.7', 8234))
    print("已成功连接 192.168.0.7:8234！开始逐路点动 (每路点动 0.4 秒)...")

    for ch in range(1, 9):
        print(f">>> 正在测试通道 {ch} - 【{names[ch-1]}】: 吸合...")
        control_channel(s, ch, True)
        time.sleep(0.4)
        print(f"    通道 {ch} 复位断开。")
        control_channel(s, ch, False)
        time.sleep(0.2)

    print("\n>>> 全部通道 (Y1~Y8) 轮巡测试完毕！所有回路已复位断开。")
    s.close()

if __name__ == "__main__":
    main()
