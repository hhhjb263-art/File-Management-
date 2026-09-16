#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CloudVault keep-alive / pipelining / 分帧健壮性 验收测试。

验证 server/src/net/http_server.cpp 的 keep-alive 实现，覆盖五类边界：

  1. 同一 TCP 段内两个 GET（pipelining）→ 各自返回一个响应，且第一个为 keep-alive；
  2. Content-Length: N（带 body）的 POST 紧跟一个 GET → 第一个请求的 body 边界正确；
  3. Content-Length: 0 的 POST 紧跟一个 GET → 零长 body 的边界切分正确；
  4. 单连接请求上限（默认 200）：逐个发 201 个 GET /healthz，前 200 个正常，
     第 201 个必须无法复用（连接已被服务端关闭，读取返回 EOF）；
  5. 分帧严格性（raw socket）：
     (a) Transfer-Encoding: chunked → 服务端不应把它当定长帧接受（非 2xx，或直接关闭）；
     (b) 重复 Content-Length → 报告实际行为，仅当出现“请求走私式错位”（第二个请求
         的响应异常）才判失败。

前置条件
--------
- 服务端已在 <host>:<port> 运行（本脚本只能在 Linux/Debian 上对真实服务端运行）。
- 若服务端以 --auth-token=<X> 启动，请用 --token X 传入；用例 2/3/5 的 POST 需要鉴权，
  未传 token 时会得到 401（且鉴权失败会关闭连接，导致 pipelining 用例失败）。
  /healthz 始终豁免。

用法
----
  python3 keepalive_test.py
  python3 keepalive_test.py --host 127.0.0.1 --port 8080
  python3 keepalive_test.py --port 8443 --token s3cr3t

退出码：全部通过 0；任一失败 1。
"""

import argparse
import socket
import sys

KMAX = 200  # 与服务端 http_server.cpp 的 kMaxRequestsPerConn 保持一致


def status_codes(buf):
    """从响应字节流中抽取每个响应的状态码（'HTTP/1.1 ' 只出现在状态行）。"""
    return [part[:3].decode("latin1", "replace") for part in buf.split(b"HTTP/1.1 ")[1:]]


def first_response_block(buf):
    parts = buf.split(b"HTTP/1.1 ")
    return b"HTTP/1.1 " + parts[1] if len(parts) > 1 else b""


def recv_responses(sock, want, timeout):
    sock.settimeout(timeout)
    buf = b""
    try:
        while buf.count(b"HTTP/1.1 ") < want:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf


def exchange(host, port, payload, want, timeout):
    """建立连接 → 单次 sendall（尽量放进同一 TCP 段）→ 读取直到收到 want 个响应或超时。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(payload)
        return recv_responses(sock, want, timeout)
    finally:
        sock.close()


def read_one_response(sock, holder, timeout):
    """按 Content-Length 精确读取一个响应，跨调用保留多余字节。

    返回 (status, head, body)；连接被对端关闭或超时 → 返回 None。
    """
    sock.settimeout(timeout)
    data = holder.get("b", b"")
    while b"\r\n\r\n" not in data:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            return None
        if not chunk:
            holder["b"] = data
            return None
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)

    clen = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            try:
                clen = int(line.split(b":", 1)[1].strip())
            except ValueError:
                clen = 0
            break
    while len(rest) < clen:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        rest += chunk

    body = rest[:clen]
    holder["b"] = rest[clen:]
    first_line = head.split(b"\r\n", 1)[0].split(b" ")
    status = first_line[1].decode("latin1", "replace") if len(first_line) > 1 else "000"
    return status, head, body


def main():
    ap = argparse.ArgumentParser(description="CloudVault keep-alive / pipelining 验收测试")
    ap.add_argument("--host", default="127.0.0.1", help="服务端地址（默认 127.0.0.1）")
    ap.add_argument("--port", type=int, default=8080, help="服务端端口（默认 8080）")
    ap.add_argument("--token", default="", help="Bearer Token（服务端启用鉴权时必填）")
    ap.add_argument("--timeout", type=float, default=5.0, help="单次读取超时秒数（默认 5）")
    args = ap.parse_args()

    auth = "Authorization: Bearer %s\r\n" % args.token if args.token else ""
    results = []

    def check(name, ok, expected, actual):
        results.append(ok)
        print("[%s] %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            print("       期望: %s" % expected)
            print("       实得: %s" % actual)

    print("== CloudVault keep-alive 验收测试  target=%s:%d token=%s =="
          % (args.host, args.port, "yes" if args.token else "no"))

    # ---- 用例 1：同段两个 GET /healthz ----
    p1 = ("GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n"
          "GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "Connection: close\r\n\r\n").encode()
    buf = exchange(args.host, args.port, p1, 2, args.timeout)
    codes = status_codes(buf)
    check("pipelining: 两个 GET 同段返回两个响应", len(codes) == 2, "2 个响应", "%d 个 (%s)" % (len(codes), codes))
    check("pipelining: 两个响应均为 200", codes[:2] == ["200", "200"], "['200', '200']", codes[:2])
    check("pipelining: 首响应 Connection: keep-alive",
          b"connection: keep-alive" in first_response_block(buf).lower(),
          "first response 含 'Connection: keep-alive'",
          first_response_block(buf)[:120])

    # ---- 用例 2：Content-Length: 5 的 POST 紧跟 GET ----
    p2 = ("POST /api/v1/files HTTP/1.1\r\nHost: localhost\r\n" + auth +
          "X-CV-Name: keepalive_test.txt\r\nContent-Length: 5\r\n\r\nhello"
          "GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "Connection: close\r\n\r\n").encode()
    buf = exchange(args.host, args.port, p2, 2, args.timeout)
    codes = status_codes(buf)
    check("CL:N body 边界: POST(5B)+GET 返回两个响应", len(codes) == 2, "2 个响应", "%d 个 (%s)" % (len(codes), codes))
    check("CL:N body 边界: 末响应为 GET /healthz 200", codes[-1:] == ["200"], "['200']", codes[-1:])

    # ---- 用例 3：Content-Length: 0 的 POST 紧跟 GET ----
    p3 = ("POST /api/v1/dirs HTTP/1.1\r\nHost: localhost\r\n" + auth + "Content-Length: 0\r\n\r\n"
          "GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "Connection: close\r\n\r\n").encode()
    buf = exchange(args.host, args.port, p3, 2, args.timeout)
    codes = status_codes(buf)
    check("CL:0 边界: POST(0B)+GET 返回两个响应", len(codes) == 2, "2 个响应", "%d 个 (%s)" % (len(codes), codes))
    check("CL:0 边界: 末响应为 GET /healthz 200", codes[-1:] == ["200"], "['200']", codes[-1:])

    # ---- 用例 4：单连接请求上限 ----
    req = ("GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "\r\n").encode()
    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    holder = {"b": b""}
    served = 0
    last_head = None
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for _ in range(KMAX):
            sock.sendall(req)
            r = read_one_response(sock, holder, args.timeout)
            if r is None:
                break
            status, head, _body = r
            if status == "200":
                served += 1
            last_head = head
        check("上限: 前 %d 个请求均返回 200" % KMAX, served == KMAX, "%d" % KMAX, served)
        check("上限: 第 %d 个响应含 Connection: close" % KMAX,
              last_head is not None and b"connection: close" in last_head.lower(),
              "含 'Connection: close'", (last_head or b"")[:120])
        # 第 201 个：发送可能成功（缓冲），读取必须失败（连接已关闭）
        try:
            sock.sendall(req)
        except OSError:
            pass
        r = read_one_response(sock, holder, min(args.timeout, 2.0))
        check("上限: 第 %d 个请求无法复用（连接已关闭）" % (KMAX + 1),
              r is None, "读取失败 / EOF", r if r is None else r[0])
    finally:
        sock.close()

    # ---- 用例 5(a)：Transfer-Encoding: chunked 必须被拒绝 ----
    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    holder = {"b": b""}
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        chunked = ("POST /api/v1/files HTTP/1.1\r\nHost: localhost\r\n" + auth +
                   "X-CV-Name: te_test.txt\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n0\r\n\r\n").encode()
        sock.sendall(chunked)
        r = read_one_response(sock, holder, args.timeout)
        if r is None:
            check("分帧: Transfer-Encoding: chunked 不被接受", True,
                  "4xx/5xx 或直接关闭", "无响应 / EOF（连接关闭）")
        else:
            st = r[0]
            accepted = st.startswith("2")
            check("分帧: Transfer-Encoding: chunked 不被接受", not accepted,
                  "非 2xx（应 4xx/5xx）", st)
    finally:
        sock.close()

    # ---- 用例 5(b)：重复 Content-Length（报告 + 走私式错位检测）----
    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    holder = {"b": b""}
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        dup = ("POST /api/v1/files HTTP/1.1\r\nHost: localhost\r\n" + auth +
               "X-CV-Name: dup_test.txt\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello"
               "GET /healthz HTTP/1.1\r\nHost: localhost\r\n" + auth + "Connection: close\r\n\r\n").encode()
        sock.sendall(dup)
        r1 = read_one_response(sock, holder, args.timeout)
        r2 = read_one_response(sock, holder, args.timeout)
        st1 = r1[0] if r1 else "<none>"
        st2 = r2[0] if r2 else "<none>"
        print("       观测: 重复 Content-Length → 第1响应=%s  第2响应=%s" % (st1, st2))
        check("分帧: 重复 Content-Length 无走私式错位（第2个请求仍解析为 200）",
              st2 == "200", "第二个响应 200", st2)
    finally:
        sock.close()

    passed = sum(1 for r in results if r)
    total = len(results)
    print("-----------------------------------------")
    print("%d/%d 检查通过" % (passed, total))
    if passed == total:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
