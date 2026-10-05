#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
中盛科技 8路以太网继电器模块 (ZS-DIO-R-10A-8) Modbus-TCP 控制驱动
支持火花模块 (Y1) 与 6路水流模块 (Y2~Y8) 独立与联动触发
"""

import socket
import struct
import time
import sys

class ModbusRelayController:
    """
    中盛科技 ZS-DIO-R-10A-8 8路继电器控制器 (Modbus-TCP)
    通道映射：
      Y1 (线圈0): 碳刷打火告警 (Spark Alarm)
      Y2 (线圈1): 冷却水系统总告警 (Water Total Alarm)
      Y3 (线圈2): Pipe 1 断水告警
      Y4 (线圈3): Pipe 2 断水告警
      Y5 (线圈4): Pipe 3 断水告警
      Y6 (线圈5): Pipe 4 断水告警
      Y7 (线圈6): Pipe 5 断水告警
      Y8 (线圈7): Pipe 6 断水告警
    """
    def __init__(self, host="192.168.1.200", port=502, timeout=2.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.trans_id = 0

    def _send_cmd(self, pdu: bytes) -> bytes:
        """封装 Modbus-TCP MBAP 报文头并发送"""
        self.trans_id = (self.trans_id + 1) & 0xFFFF
        # MBAP: Transaction ID (2B), Protocol ID (2B, 0=Modbus), Length (2B), Unit ID (1B, default 1)
        length = len(pdu) + 1
        header = struct.pack(">HHHB", self.trans_id, 0, length, 1)
        packet = header + pdu

        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(self.timeout)
                s.connect((self.host, self.port))
                s.sendall(packet)
                resp = s.recv(1024)
                return resp
        except Exception as e:
            print(f"[ModbusRelay] 通信失败 ({self.host}:{self.port}): {e}", file=sys.stderr)
            return b""

    def write_single_coil(self, channel: int, state: bool) -> bool:
        """
        写单个线圈 (0x05 功能码)
        :param channel: 继电器通道 1 ~ 8 (对应 Y1 ~ Y8)
        :param state: True 闭合吸合 (0xFF00), False 断开释放 (0x0000)
        """
        if not (1 <= channel <= 8):
            raise ValueError("通道号必须在 1 ~ 8 之间 (对应 Y1 ~ Y8)")
        
        coil_addr = channel - 1
        coil_val = 0xFF00 if state else 0x0000
        pdu = struct.pack(">BHH", 0x05, coil_addr, coil_val)
        resp = self._send_cmd(pdu)
        return len(resp) >= 12

    def trigger_pulse(self, channel: int, duration_sec: float = 1.0):
        """
        点动脉冲触发：常开端闭合指定秒数后自动释放复位
        """
        print(f"[ModbusRelay] 触发继电器 Y{channel} 闭合告警 (脉冲 {duration_sec}s)...")
        self.write_single_coil(channel, True)
        time.sleep(duration_sec)
        self.write_single_coil(channel, False)
        print(f"[ModbusRelay] 继电器 Y{channel} 已自动复位断开。")

    def reset_all(self):
        """全部继电器断开复位 (写多线圈 0x0F)"""
        # 功能码 0x0F, 起始地址 0x0000, 数量 8, 字节数 1, 状态值 0x00
        pdu = struct.pack(">BHHBB", 0x0F, 0x0000, 8, 1, 0x00)
        self._send_cmd(pdu)
        print("[ModbusRelay] 所有继电器通道 (Y1~Y8) 已复位断开。")


if __name__ == "__main__":
    print("==================================================")
    print("  中盛科技 8路以太网继电器 (ZS-DIO-R-10A-8) 测试")
    print("==================================================")
    # 默认现场 IP，可根据实际设置调整
    target_ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.200"
    relay = ModbusRelayController(host=target_ip)
    
    print(f"正在尝试连接模块 IP: {target_ip}:502 ...")
    # 测试通道 2 (冷却水总警报) 脉冲 1 秒
    relay.trigger_pulse(channel=2, duration_sec=1.0)
