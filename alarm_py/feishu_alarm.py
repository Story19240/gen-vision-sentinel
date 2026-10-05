#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
发电机智能视觉预警系统 - 飞书群自定义机器人推送核心模块
支持：
  1. 冷却水单管断水/堵塞紧急报警 (红底卡片)
  2. 冷却水疑似流量波动预警 (橙底卡片)
  3. 水流恢复正常通知 (绿底卡片)
  4. 碳刷火花打火报警 (红底卡片)
  5. 现场大屏一键跳转与签名安全验证
"""

import os
import sys
import json
import time
import hmac
import hashlib
import base64
import argparse
import requests
from datetime import datetime

# Windows 终端控制台 utf-8 编码兼容
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    except Exception:
        pass

# 默认配置文件路径

CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_FILE = os.path.join(CURRENT_DIR, "feishu_config.json")

class FeishuAlarmClient:
    def __init__(self, config_path=CONFIG_FILE):
        self.config_path = config_path
        self.config = self._load_config()
        self.webhook_url = self.config.get("webhook_url", "").strip()
        self.secret = self.config.get("secret", "").strip()
        self.app_id = self.config.get("app_id", "").strip()
        self.app_secret = self.config.get("app_secret", "").strip()
        self.receive_id_type = self.config.get("receive_id_type", "chat_id").strip()
        self.receive_id = self.config.get("receive_id", "").strip()
        self.device_name = self.config.get("device_name", "1号发电机机组 - 智能视觉防护系统")
        self.web_monitor_url = self.config.get("web_monitor_url", "http://192.168.55.1:8080")
        self._token = ""
        self._token_expire_time = 0

    def _load_config(self):
        """读取外部配置文件，方便现场免改代码修改 Webhook 与 App 凭证"""
        if os.path.exists(self.config_path):
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    return json.load(f)
            except Exception as e:
                print(f"[FeishuAlarm] 读取配置文件失败: {e}", file=sys.stderr)
        return {}

    def get_tenant_access_token(self) -> str:
        """获取飞书自建应用的 tenant_access_token"""
        if self._token and time.time() < self._token_expire_time:
            return self._token

        if not self.app_id or not self.app_secret or "在此填入" in self.app_secret:
            return ""

        url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal"
        payload = {
            "app_id": self.app_id,
            "app_secret": self.app_secret
        }
        try:
            resp = requests.post(url, json=payload, timeout=5.0)
            data = resp.json()
            if data.get("code") == 0:
                self._token = data.get("tenant_access_token", "")
                self._token_expire_time = time.time() + data.get("expire", 7100) - 100
                return self._token
            else:
                print(f"[FeishuAlarm] 获取自建应用 Token 失败: {data}", file=sys.stderr)
                return ""
        except Exception as e:
            print(f"[FeishuAlarm] 获取自建应用 Token 网络异常: {e}", file=sys.stderr)
            return ""

    def list_bot_chats(self) -> list:
        """列出机器人所在的所有群聊列表及 chat_id"""
        token = self.get_tenant_access_token()
        if not token:
            return []
        url = "https://open.feishu.cn/open-apis/im/v1/chats?page_size=20"
        headers = {"Authorization": f"Bearer {token}"}
        try:
            resp = requests.get(url, headers=headers, timeout=5.0)
            data = resp.json()
            if data.get("code") == 0:
                return data.get("data", {}).get("items", [])
        except Exception:
            pass
        return []

    def _generate_sign(self, timestamp: int) -> str:
        """飞书群机器人签名安全验证算法 (HMAC-SHA256)"""
        if not self.secret:
            return ""
        string_to_sign = f"{timestamp}\n{self.secret}"
        hmac_code = hmac.new(
            string_to_sign.encode("utf-8"),
            digestmod=hashlib.sha256
        ).digest()
        return base64.b64encode(hmac_code).decode("utf-8")


    def send_card(self, title: str, template_color: str, fields: list, action_btn_text: str = "查看现场实时大屏") -> bool:
        """
        发送飞书富文本交互式卡片 (自动识别 自建应用模式 或 Webhook 模式)
        :param title: 卡片标题
        :param template_color: 卡片标题颜色 ('red', 'orange', 'green', 'blue', 'wathet')
        :param fields: 键值对列表 [{"label": "...", "value": "..."}, ...]
        :param action_btn_text: 底部跳转按钮文本
        """
        current_time_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


        # 构建字段列表 (两列排布)
        field_elements = []
        for item in fields:
            field_elements.append({
                "is_short": True,
                "text": {
                    "tag": "lark_md",
                    "content": f"**{item['label']}：**\n{item['value']}"
                }
            })

        # 构建卡片主体
        elements = [
            {
                "tag": "div",
                "fields": field_elements
            },
            {
                "tag": "hr"
            },
            {
                "tag": "note",
                "elements": [
                    {
                        "tag": "plain_text",
                        "content": f"⏱ 触发时间: {current_time_str} | 设备监控哨兵已自动存证"
                    }
                ]
            }
        ]

        # 底部附带 Web 监控大屏跳转按钮
        if self.web_monitor_url:
            elements.append({
                "tag": "action",
                "actions": [
                    {
                        "tag": "button",
                        "text": {
                            "tag": "plain_text",
                            "content": f"🔍 {action_btn_text}"
                        },
                        "type": "primary" if template_color == "red" else "default",
                        "url": self.web_monitor_url
                    }
                ]
            })

        card_payload = {
            "config": {
                "wide_screen_mode": True
            },
            "header": {
                "title": {
                    "tag": "plain_text",
                    "content": title
                },
                "template": template_color  # 标题栏颜色
            },
            "elements": elements
        }

        token = self.get_tenant_access_token()

        # 方式 1: 通过企业自建应用 (App ID + Secret) 发送消息
        if token:
            target_id = self.receive_id
            target_type = self.receive_id_type

            # 若未指定 receive_id，自动查找机器人所在群
            if not target_id:
                chats = self.list_bot_chats()
                if chats:
                    target_id = chats[0].get("chat_id")
                    target_type = "chat_id"
                    print(f"[FeishuAlarm] 自动探测到机器人所在群: {chats[0].get('name')} (chat_id: {target_id})")
                else:
                    print("[FeishuAlarm] 提示: 请在飞书群中将自建机器人拉入群聊，或在 feishu_config.json 中配置 receive_id！")
                    return False

            send_url = f"https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type={target_type}"
            headers = {
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json; charset=utf-8"
            }
            body = {
                "receive_id": target_id,
                "msg_type": "interactive",
                "content": json.dumps(card_payload)
            }
            try:
                resp = requests.post(send_url, json=body, headers=headers, timeout=5.0)
                res = resp.json()
                if res.get("code") == 0:
                    print(f"[FeishuAlarm] 自建应用发送卡片成功: {title}")
                    return True
                else:
                    print(f"[FeishuAlarm] 自建应用发送失败: {res}", file=sys.stderr)
                    return False
            except Exception as e:
                print(f"[FeishuAlarm] 自建应用发送网络异常: {e}", file=sys.stderr)
                return False

        # 方式 2: 通过群自定义机器人 Webhook 发送
        if self.webhook_url and "YOUR_WEBHOOK" not in self.webhook_url:
            timestamp = int(time.time())
            post_data = {
                "msg_type": "interactive",
                "card": card_payload
            }
            if self.secret:
                post_data["timestamp"] = str(timestamp)
                post_data["sign"] = self._generate_sign(timestamp)

            try:
                headers = {"Content-Type": "application/json; charset=utf-8"}
                resp = requests.post(self.webhook_url, json=post_data, headers=headers, timeout=5.0)
                result = resp.json()
                if result.get("code") == 0 or result.get("StatusCode") == 0:
                    print(f"[FeishuAlarm] Webhook 卡片推送成功: {title}")
                    return True
                else:
                    print(f"[FeishuAlarm] Webhook 推送失败，飞书返回: {result}", file=sys.stderr)
                    return False
            except Exception as e:
                print(f"[FeishuAlarm] Webhook 网络请求异常: {e}", file=sys.stderr)
                return False

        print("[FeishuAlarm] 警告: 未配置有效的 App Secret 或 Webhook，请在 feishu_config.json 中填入！")
        return False


    # =========================================================================
    # 核心业务专用推送方法
    # =========================================================================

    def notify_water_flow_alarm(self, pipe_id: int, pipe_name: str, current_energy: float, 
                                threshold: float, duration_sec: float, relay_channel: str = None) -> bool:
        """
        冷却水单管断水 / 严重堵塞紧急告警 (红底卡片)
        """
        title = f"🔴【紧急告警】发电机冷却水断流 - Pipe {pipe_id} 异常！"
        relay_info = relay_channel if relay_channel else f"DO{pipe_id + 2} (端子已闭合输出)"

        fields = [
            {"label": "监控设备", "value": self.device_name},
            {"label": "告警等级", "value": "🚨 一级紧急 (CRITICAL)"},
            {"label": "故障管道", "value": f"**Pipe {pipe_id}** ({pipe_name})"},
            {"label": "异常状态", "value": f"水流完全中断 / 流量趋近于零"},
            {"label": "实时水流动能", "value": f"`{current_energy:.2f}` (安全阈值: {threshold:.2f})"},
            {"label": "持续异常时间", "value": f"**{duration_sec:.1f} 秒** (超过设定上限)"},
            {"label": "硬件联锁动作", "value": f"`{relay_info}`"},
            {"label": "处置建议", "value": "请值班人员立即核实备用水泵或联系机组降负荷！"}
        ]
        return self.send_card(title, template_color="red", fields=fields)

    def notify_water_warning(self, pipe_id: int, pipe_name: str, current_energy: float, 
                             threshold: float, duration_sec: float) -> bool:
        """
        冷却水疑似气阻或波动预警 (橙底卡片)
        """
        title = f"⚠️【运行预警】冷却水流量偏低 - Pipe {pipe_id} 疑似气阻"
        fields = [
            {"label": "监控设备", "value": self.device_name},
            {"label": "预警等级", "value": "⚠️ 二级注意 (WARNING)"},
            {"label": "预警管道", "value": f"**Pipe {pipe_id}** ({pipe_name})"},
            {"label": "实时水流动能", "value": f"`{current_energy:.2f}` (安全阈值: {threshold:.2f})"},
            {"label": "波动持续时间", "value": f"{duration_sec:.1f} 秒"},
            {"label": "状态说明", "value": "水流短时跌落，系统正在密切跟踪，若持续超 2s 将升格停机报警"}
        ]
        return self.send_card(title, template_color="orange", fields=fields)

    def notify_water_recovery(self, pipe_id: int, pipe_name: str, current_energy: float) -> bool:
        """
        冷却水恢复供水通知 (绿底卡片)
        """
        title = f"🟢【状态恢复】发电机冷却水正常 - Pipe {pipe_id} 水流恢复"
        fields = [
            {"label": "监控设备", "value": self.device_name},
            {"label": "通知等级", "value": "✅ 正常运行 (NORMAL)"},
            {"label": "恢复管道", "value": f"**Pipe {pipe_id}** ({pipe_name})"},
            {"label": "当前动能参数", "value": f"`{current_energy:.2f}` (已回归健康区间)"},
            {"label": "继电器状态", "value": "对应告警回路已自动释放复位"}
        ]
        return self.send_card(title, template_color="green", fields=fields)

    def notify_spark_alarm(self, confidence: float, duration_ms: float, relay_channel: str = "DO1") -> bool:
        """
        发电机碳刷火花打火告警 (红底卡片)
        """
        title = "🔴【紧急告警】发电机碳刷打火 - 滑环接触面电弧异常！"
        fields = [
            {"label": "监控设备", "value": self.device_name},
            {"label": "告警等级", "value": "🚨 一级紧急 (CRITICAL)"},
            {"label": "故障部位", "value": "转子励磁滑环与碳刷接触摩擦面"},
            {"label": "打火置信度", "value": f"**{confidence * 100:.1f}%**"},
            {"label": "打火持续时间", "value": f"**{duration_ms:.0f} 毫秒** (超过 200ms 安全上限)"},
            {"label": "硬件联锁动作", "value": f"`{relay_channel}` (碳刷跳闸回路已闭合)"},
            {"label": "处置建议", "value": "严防滑环灼伤，请立即排查碳刷磨损极限或卡涩跳动！"}
        ]
        return self.send_card(title, template_color="red", fields=fields)


# =============================================================================
# 命令行测试与演示入口
# =============================================================================
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="发电机智能视觉防护系统 - 飞书机器人推送测试工具")
    parser.add_argument("--test-water", action="store_true", help="模拟触发一次【2号水管断流】红色紧急告警")
    parser.add_argument("--test-warn", action="store_true", help="模拟触发一次【2号水管流量偏低】橙色预警")
    parser.add_argument("--test-recovery", action="store_true", help="模拟触发一次【2号水管恢复供水】绿色通知")
    parser.add_argument("--test-spark", action="store_true", help="模拟触发一次【发电机碳刷打火】红色紧急告警")
    parser.add_argument("--webhook", type=str, default="", help="临时指定的 Webhook URL（若不传则从配置文件读取）")

    args = parser.parse_args()
    client = FeishuAlarmClient()

    if args.webhook:
        client.webhook_url = args.webhook

    print("==================================================")
    print("  发电机视觉预警系统 - 飞书推送客户端")
    print(f"  当前配置的目标设备: {client.device_name}")
    print(f"  当前配置的 Webhook: {client.webhook_url[:35]}..." if len(client.webhook_url) > 35 else f"  当前 Webhook: {client.webhook_url}")
    print("==================================================")

    if args.test_water:
        print("正在发送【冷却水断水报警】模拟卡片...")
        client.notify_water_flow_alarm(
            pipe_id=2, 
            pipe_name="细黄管(直流水)", 
            current_energy=0.08, 
            threshold=3.50, 
            duration_sec=2.4
        )
    elif args.test_warn:
        print("正在发送【冷却水流量偏低预警】模拟卡片...")
        client.notify_water_warning(
            pipe_id=2, 
            pipe_name="细黄管(直流水)", 
            current_energy=2.10, 
            threshold=3.50, 
            duration_sec=0.8
        )
    elif args.test_recovery:
        print("正在发送【冷却水恢复正常】模拟卡片...")
        client.notify_water_recovery(
            pipe_id=2, 
            pipe_name="细黄管(直流水)", 
            current_energy=11.20
        )
    elif args.test_spark:
        print("正在发送【发电机碳刷打火报警】模拟卡片...")
        client.notify_spark_alarm(
            confidence=0.92, 
            duration_ms=350.0
        )
    else:
        print("\n[使用指南]：")
        print("1. 请在 alarm_py/feishu_config.json 中填入真实的飞书机器人 Webhook 地址")
        print("2. 运行模拟测试命令：")
        print("   python feishu_alarm.py --test-water     # 模拟 2 号水管断流紧急报警")
        print("   python feishu_alarm.py --test-warn      # 模拟水流偏低预警")
        print("   python feishu_alarm.py --test-recovery  # 模拟水流恢复通知")
        print("   python feishu_alarm.py --test-spark     # 模拟碳刷打火告警")

