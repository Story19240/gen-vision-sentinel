import requests
import json
import base64
import time

def send_feishu_alarm(webhook_url, title, content_dict, image_path=None):
    """
    发送飞书富文本/交互卡片报警消息
    :param webhook_url: 飞书自定义群机器人 Webhook 链接
    :param title: 卡片标题 (如: 🔴【发电机紧急告警】冷却水断流)
    :param content_dict: 告警字段字典 (如设备名、管道号、时间戳等)
    :param image_path: 可选，带红框的高清报警抓拍图路径
    """
    elements = []
    
    # 构建文字字段
    fields_list = []
    for k, v in content_dict.items():
        fields_list.append({
            "is_short": True,
            "text": {
                "tag": "lark_md",
                "content": f"**{k}:**\n{v}"
            }
        })
    
    elements.append({
        "tag": "div",
        "fields": fields_list
    })
    
    # 底部提示
    elements.append({
        "tag": "hr"
    })
    elements.append({
        "tag": "note",
        "elements": [
            {
                "tag": "plain_text",
                "content": "⚠️ 硬件开关量回路已同步动作，请现场值班人员立即核实处置！"
            }
        ]
    })

    card = {
        "config": {
            "wide_screen_mode": True
        },
        "header": {
            "title": {
                "tag": "plain_text",
                "content": title
            },
            "template": "red"  # 红色警戒标题栏
        },
        "elements": elements
    }

    payload = {
        "msg_type": "interactive",
        "card": card
    }

    try:
        resp = requests.post(webhook_url, json=payload, timeout=5)
        print("飞书推送响应:", resp.status_code, resp.text)
        return resp.status_code == 200
    except Exception as e:
        print("飞书推送失败:", e)
        return False

if __name__ == "__main__":
    # 测试示例 (等待客户提供真实 Webhook 链接)
    dummy_webhook = "https://open.feishu.cn/open-apis/bot/v2/hook/YOUR-FEISHU-WEBHOOK-TOKEN"
    print("飞书报警推送模块已就绪。当客户提供真实 Webhook 链接后即可直接调用。")
