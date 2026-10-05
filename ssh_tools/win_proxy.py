import socket
import select
import sys
import threading

def handle_client(client_socket):
    try:
        request = client_socket.recv(8192)
        if not request:
            client_socket.close()
            return
        
        first_line = request.split(b'\n')[0]
        words = first_line.split()
        if len(words) < 2:
            client_socket.close()
            return
        
        method = words[0].decode('latin1', 'ignore')
        target = words[1].decode('latin1', 'ignore')
        
        if method.upper() == 'CONNECT':
            host, port = target.split(':')
            port = int(port)
            remote = socket.create_connection((host, port), timeout=10)
            client_socket.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        else:
            # 标准 HTTP GET/POST 代理
            if target.startswith('http://'):
                url_parts = target[7:].split('/', 1)
                host_port = url_parts[0]
            else:
                host_port = target
            if ':' in host_port:
                host, port = host_port.split(':')
                port = int(port)
            else:
                host = host_port
                port = 80
            remote = socket.create_connection((host, port), timeout=10)
            remote.sendall(request)
        
        # 双向转发
        sockets = [client_socket, remote]
        while True:
            r, _, _ = select.select(sockets, [], [], 30)
            if not r:
                break
            for s in r:
                other = remote if s is client_socket else client_socket
                data = s.recv(8192)
                if not data:
                    return
                other.sendall(data)
    except Exception:
        pass
    finally:
        try: client_socket.close()
        except: pass
        try: remote.close()
        except: pass

def main():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # 监听在 192.168.55.100 (给开发板提供上网通道) 和 0.0.0.0
    server.bind(('0.0.0.0', 10811))
    server.listen(50)
    print("Windows HTTP Forwarding Proxy running on port 10811...")
    sys.stdout.flush()
    while True:
        client, addr = server.accept()
        t = threading.Thread(target=handle_client, args=(client,), daemon=True)
        t.start()

if __name__ == '__main__':
    main()
