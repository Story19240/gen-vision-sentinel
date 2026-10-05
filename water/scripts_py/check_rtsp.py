import os
import socket
from urllib.parse import urlsplit

rtsp_url = os.environ.get("RTSP_URL")
if not rtsp_url:
    raise SystemExit("请先设置 RTSP_URL 环境变量，例如 rtsp://username:password@camera-host:554/path")

parsed = urlsplit(rtsp_url)
s = socket.socket()
s.connect((parsed.hostname, parsed.port or 554))
req = f"DESCRIBE {rtsp_url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n"
s.send(req.encode())
res = s.recv(4096)
print(res.decode(errors="ignore"))
s.close()
