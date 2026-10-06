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

# 继电器 2 (Y2) 闭合与断开指令
cmd_on_y2 = crc16(bytes([0x01, 0x05, 0x00, 0x01, 0xFF, 0x00]))
cmd_off_y2 = crc16(bytes([0x01, 0x05, 0x00, 0x01, 0x00, 0x00]))

print(f"Y2 ON  Hex: {cmd_on_y2.hex(' ')}")
print(f"Y2 OFF Hex: {cmd_off_y2.hex(' ')}")

try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    print("Connecting to 192.168.0.7:8234 ...")
    s.connect(('192.168.0.7', 8234))
    print("Connected successfully!")

    print(">>> 触发 Y2 (冷却水总报警) 闭合...")
    s.sendall(cmd_on_y2)
    resp = s.recv(1024)
    print(f"收到模块响应: {resp.hex(' ')}")

    print("保持吸合 1.5 秒...")
    time.sleep(1.5)

    print(">>> 触发 Y2 断开复位...")
    s.sendall(cmd_off_y2)
    resp = s.recv(1024)
    print(f"收到模块响应: {resp.hex(' ')}")

    s.close()
    print("=== 测试圆满成功！ ===")
except Exception as e:
    print(f"连接或通信异常: {e}")
