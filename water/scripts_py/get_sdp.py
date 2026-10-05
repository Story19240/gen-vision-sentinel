import hashlib
import os
import re
import socket
from urllib.parse import unquote, urlsplit

rtsp_url = os.environ.get("RTSP_URL")
if not rtsp_url:
    raise SystemExit("请先设置 RTSP_URL 环境变量，例如 rtsp://username:password@camera-host:554/path")

parsed = urlsplit(rtsp_url)
username = unquote(parsed.username or "")
password = unquote(parsed.password or "")

s = socket.socket()
s.connect((parsed.hostname, parsed.port or 554))

req1 = f"DESCRIBE {rtsp_url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n"
s.send(req1.encode())
res1 = s.recv(4096).decode(errors="ignore")

realm = re.search(r'realm="([^"]+)"', res1).group(1)
nonce = re.search(r'nonce="([^"]+)"', res1).group(1)

ha1 = hashlib.md5(f"{username}:{password}:{realm}".encode()).hexdigest()
ha2 = hashlib.md5(f"DESCRIBE:{rtsp_url}".encode()).hexdigest()
response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()

auth_header = (
    f'Digest username="{username}", realm="{realm}", nonce="{nonce}", '
    f'uri="{rtsp_url}", response="{response}"'
)
req2 = (
    f"DESCRIBE {rtsp_url} RTSP/1.0\r\nCSeq: 2\r\n"
    f"Authorization: {auth_header}\r\nAccept: application/sdp\r\n\r\n"
)
s.send(req2.encode())
res2 = s.recv(4096).decode(errors="ignore")
print(res2)
s.close()
