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
        while b"\r\n\r\n" not in response:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk

        if b"\r\n\r\n" in response:
            hdr, body = response.split(b"\r\n\r\n", 1)
            clen = None
            for line in hdr.split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    clen = int(line.split(b":", 1)[1].strip())
                    break
            if clen is not None:
                while len(body) < clen:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    body += chunk
                response = hdr + b"\r\n\r\n" + body
            else:
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

def test_get_static_http11(port):
    print("Testing HTTP/1.1 GET / (static index.html)...", end=" ")
    req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    resp = send_raw(port, req)
    assert "HTTP/1.1 200 OK" in resp, f"Expected 200 OK, got: {resp[:100]}"
    assert "Server: slew/1.1" in resp, f"Expected Server slew/1.1, got: {resp[:100]}"
    assert "Date: " in resp, "Expected Date header"
    assert "Connection: keep-alive" in resp
    assert "slew Web Server" in resp
    print("PASSED")

def test_head_static_http11(port):
    print("Testing HTTP/1.1 HEAD /...", end=" ")
    req = "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    resp = send_raw(port, req)
    assert "HTTP/1.1 200 OK" in resp
    assert "Server: slew/1.1" in resp
    parts = resp.split("\r\n\r\n", 1)
    assert len(parts) == 2
    assert parts[1] == "", f"HEAD body must be empty, got {len(parts[1])} bytes"
    print("PASSED")

def test_dynamic_get_http11(port):
    print("Testing dynamic GET /health HTTP/1.1...", end=" ")
    req = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"
    resp = send_raw(port, req)
    assert "HTTP/1.1 200 OK" in resp
    assert "application/json" in resp
    assert '{"status":"ok","server":"slew/1.1"}' in resp
    print("PASSED")

def test_dynamic_post_http11(port):
    print("Testing dynamic POST /api/echo HTTP/1.1...", end=" ")
    payload = "Hello HTTP/1.1 io_uring server!"
    req = (
        f"POST /api/echo HTTP/1.1\r\n"
        f"Host: localhost\r\n"
        f"Content-Type: text/plain\r\n"
        f"Content-Length: {len(payload)}\r\n\r\n"
        f"{payload}"
    )
    resp = send_raw(port, req)
    assert "HTTP/1.1 200 OK" in resp
    assert payload in resp
    print("PASSED")

def test_http11_default_keep_alive(port):
    print("Testing HTTP/1.1 default keep-alive on single socket...", end=" ")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(("127.0.0.1", port))

    # Request 1 (no Connection header sent, defaults to keep-alive in 1.1)
    s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n")
    resp1 = b""
    while b"\r\n\r\n" not in resp1:
        resp1 += s.recv(1024)
    hdr1, body1 = resp1.split(b"\r\n\r\n", 1)
    while len(body1) < 34:
        body1 += s.recv(1024)
    assert b"HTTP/1.1 200 OK" in hdr1
    assert b"Connection: keep-alive" in hdr1

    # Request 2 on SAME socket with explicit Connection: close
    s.sendall(b"GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    resp2 = b""
    while True:
        chunk = s.recv(1024)
        if not chunk: # EOF: server closed socket per Connection: close
            break
        resp2 += chunk
    s.close()

    assert b"HTTP/1.1 200 OK" in resp2
    assert b"Connection: close" in resp2
    print("PASSED")

def test_host_header_missing_rejected(port):
    print("Testing HTTP/1.1 400 on missing Host header...", end=" ")
    resp = send_raw(port, "GET /index.html HTTP/1.1\r\n\r\n")
    assert "HTTP/1.1 400 Bad Request" in resp
    assert "Connection: close" in resp
    print("PASSED")

def test_host_header_duplicate_rejected(port):
    print("Testing HTTP/1.1 400 on duplicate Host headers...", end=" ")
    resp = send_raw(port, "GET /index.html HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\n\r\n")
    assert "HTTP/1.1 400 Bad Request" in resp
    print("PASSED")

def test_host_header_empty_rejected(port):
    print("Testing HTTP/1.1 400 on empty Host header...", end=" ")
    resp = send_raw(port, "GET /index.html HTTP/1.1\r\nHost: \r\n\r\n")
    assert "HTTP/1.1 400 Bad Request" in resp
    print("PASSED")

def test_length_required_411(port):
    print("Testing HTTP/1.1 411 Length Required for POST without Content-Length...", end=" ")
    resp = send_raw(port, "POST /api/echo HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert "HTTP/1.1 411 Length Required" in resp
    print("PASSED")

def test_method_not_allowed_405(port):
    print("Testing HTTP/1.1 405 Method Not Allowed for PUT/DELETE on static files...", end=" ")
    resp = send_raw(port, "PUT /index.html HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n")
    assert "HTTP/1.1 405 Method Not Allowed" in resp
    assert "Allow: GET, HEAD" in resp
    print("PASSED")

def test_version_not_supported_505(port):
    print("Testing 505 HTTP Version Not Supported...", end=" ")
    resp = send_raw(port, "GET / HTTP/9.9\r\nHost: localhost\r\n\r\n")
    assert "HTTP/1.1 505 HTTP Version Not Supported" in resp
    print("PASSED")

def test_http10_compatibility(port):
    print("Testing HTTP/1.0 backwards compatibility...", end=" ")
    # HTTP/1.0 request without Host header should succeed and close connection by default
    resp = send_raw(port, "GET /health HTTP/1.0\r\n\r\n")
    assert "HTTP/1.1 200 OK" in resp
    assert "Connection: close" in resp

    # HTTP/1.0 request with Connection: keep-alive
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(("127.0.0.1", port))
    s.sendall(b"GET /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
    res = b""
    while b"\r\n\r\n" not in res:
        res += s.recv(1024)
    s.close()
    assert b"HTTP/1.1 200 OK" in res
    assert b"Connection: keep-alive" in res
    print("PASSED")

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"=== Running RFC 7230 / HTTP 1.1 Compliance Suite against localhost:{port} ===")

    test_get_static_http11(port)
    test_head_static_http11(port)
    test_dynamic_get_http11(port)
    test_dynamic_post_http11(port)
    test_http11_default_keep_alive(port)
    test_host_header_missing_rejected(port)
    test_host_header_duplicate_rejected(port)
    test_host_header_empty_rejected(port)
    test_length_required_411(port)
    test_method_not_allowed_405(port)
    test_version_not_supported_505(port)
    test_http10_compatibility(port)

    print("\nAll HTTP/1.1 compliance tests passed successfully.")

if __name__ == "__main__":
    main()
