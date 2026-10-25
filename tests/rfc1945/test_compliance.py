#!/usr/bin/env python3
import socket
import sys
import time

def send_raw(port, data, read_timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(read_timeout)
    s.connect(("127.0.0.1", port))
    s.sendall(data.encode("latin1") if isinstance(data, str) else data)

    response = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk
    except socket.timeout:
        pass
    finally:
        s.close()
    return response.decode("latin1")

def test_get_static(port):
    print("Testing GET / (static index.html)...", end=" ")
    resp = send_raw(port, "GET / HTTP/1.0\r\n\r\n")
    assert "HTTP/1.0 200 OK" in resp, f"Expected 200 OK, got: {resp[:100]}"
    assert "Content-Type: text/html" in resp
    assert "slew Web Server" in resp
    assert "Connection: close" in resp
    print("PASSED")

def test_head_static(port):
    print("Testing HEAD / (no body per RFC 1945)...", end=" ")
    resp = send_raw(port, "HEAD / HTTP/1.0\r\n\r\n")
    assert "HTTP/1.0 200 OK" in resp
    assert "Content-Type: text/html" in resp
    parts = resp.split("\r\n\r\n", 1)
    assert len(parts) == 2
    body = parts[1]
    assert body == "", f"HEAD response must have empty body, got {len(body)} bytes"
    print("PASSED")

def test_dynamic_get(port):
    print("Testing dynamic GET /health...", end=" ")
    resp = send_raw(port, "GET /health HTTP/1.0\r\n\r\n")
    assert "HTTP/1.0 200 OK" in resp
    assert "application/json" in resp
    assert '{"status":"ok","server":"slew/1.1"}' in resp
    print("PASSED")

def test_dynamic_post(port):
    print("Testing dynamic POST /api/echo...", end=" ")
    payload = "Hello, slew raw io_uring server!"
    req = (
        f"POST /api/echo HTTP/1.0\r\n"
        f"Content-Type: text/plain\r\n"
        f"Content-Length: {len(payload)}\r\n\r\n"
        f"{payload}"
    )
    resp = send_raw(port, req)
    assert "HTTP/1.0 200 OK" in resp
    assert payload in resp
    print("PASSED")

def test_not_found(port):
    print("Testing 404 Not Found...", end=" ")
    resp = send_raw(port, "GET /file_does_not_exist_404.txt HTTP/1.0\r\n\r\n")
    assert "HTTP/1.0 404 Not Found" in resp
    print("PASSED")

def test_directory_traversal_defense(port):
    print("Testing 403 Forbidden directory traversal...", end=" ")
    resp = send_raw(port, "GET /../../etc/passwd HTTP/1.0\r\n\r\n")
    assert "HTTP/1.0 403 Forbidden" in resp
    print("PASSED")

def test_method_not_allowed(port):
    print("Testing 405 Method Not Allowed...", end=" ")
    # Sending PUT on static asset
    resp = send_raw(port, "PUT /index.html HTTP/1.0\r\n\r\n")
    # PUT is either 405 or 501 depending on route match
    assert ("HTTP/1.0 405 Method Not Allowed" in resp or "HTTP/1.0 501" in resp), f"Got: {resp[:100]}"
    print("PASSED")

def test_bad_request(port):
    print("Testing 400 Bad Request...", end=" ")
    resp = send_raw(port, "MALFORMED_GARBAGE\r\n\r\n")
    assert "HTTP/1.0 400 Bad Request" in resp
    print("PASSED")

def test_version_not_supported(port):
    print("Testing 505 HTTP Version Not Supported...", end=" ")
    resp = send_raw(port, "GET / HTTP/9.9\r\n\r\n")
    assert "HTTP/1.0 505" in resp
    print("PASSED")

def test_default_close_rfc1945(port):
    print("Testing RFC 1945 default close (server closes socket)...", end=" ")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(("127.0.0.1", port))
    s.sendall(b"GET /health HTTP/1.0\r\n\r\n")

    res = b""
    while True:
        chunk = s.recv(1024)
        if not chunk: # EOF received!
            break
        res += chunk
    s.close()
    assert b"HTTP/1.0 200 OK" in res
    assert b"Connection: close" in res
    print("PASSED")

def test_keep_alive_persistence(port):
    print("Testing HTTP/1.0 Keep-Alive persistence on single socket...", end=" ")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(("127.0.0.1", port))

    # Request 1
    s.sendall(b"GET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    resp1 = b""
    while b"\r\n\r\n" not in resp1:
        resp1 += s.recv(1024)
    # Read body
    hdr, body1 = resp1.split(b"\r\n\r\n", 1)
    assert b"Connection: keep-alive" in hdr

    # Request 2 on the SAME socket
    s.sendall(b"GET /health HTTP/1.0\r\nConnection: close\r\n\r\n")
    resp2 = b""
    while True:
        chunk = s.recv(1024)
        if not chunk: # Server closes after Connection: close
            break
        resp2 += chunk
    s.close()

    assert b"HTTP/1.0 200 OK" in resp2
    assert b"Connection: close" in resp2
    print("PASSED")

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"=== Running RFC 1945 Compliance Suite against localhost:{port} ===")

    test_get_static(port)
    test_head_static(port)
    test_dynamic_get(port)
    test_dynamic_post(port)
    test_not_found(port)
    test_directory_traversal_defense(port)
    test_method_not_allowed(port)
    test_bad_request(port)
    test_version_not_supported(port)
    test_default_close_rfc1945(port)
    test_keep_alive_persistence(port)

    print("\nAll RFC 1945 compliance tests passed successfully.")

if __name__ == "__main__":
    main()
