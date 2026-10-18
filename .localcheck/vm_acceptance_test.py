#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""云匣服务端验收测试（在 VM 内运行）
覆盖：健康/鉴权 · 单块 · 分块边界 · 秒传 · 断点续传(杀进程模拟断电) ·
      块损坏检测 · 批量删除(文件+目录) · 批量移动 · 并发
"""
import hashlib, json, os, shutil, subprocess, sys, threading, time
import urllib.request, urllib.error

HOST = "http://127.0.0.1:8080"; TOKEN = "abc123"
SRV  = "/home/user/File-Management-/server"; BIN = SRV + "/build/cloudvault-server"
DATA = "/home/user/cvtest-data"; LOG = "/home/user/cvtest.log"; CERTS = SRV + "/testdata"
CHUNK = 5 * 1024 * 1024

P = []; F = []
def ck(n, c, d=""):
    (P if c else F).append(n)
    print(("  [OK] " if c else "  [FAIL] ") + n + (("  <- " + str(d)[:200]) if (d and not c) else ""))
def sec(t): print("\n=== " + t + " ===")

def req(m, p, b=None, hdrs=None, tok=TOKEN, tout=30):
    data = None
    if b is not None:
        data = b if isinstance(b, (bytes, bytearray)) else json.dumps(b).encode()
    r = urllib.request.Request(HOST + p, data=data, method=m)
    if tok: r.add_header("Authorization", "Bearer " + tok)
    if data is not None: r.add_header("Content-Type", "application/octet-stream" if hdrs else "application/json")
    for k, v in (hdrs or {}).items(): r.add_header(k, v)
    try:
        with urllib.request.urlopen(r, timeout=tout) as x: return x.status, x.read()
    except urllib.error.HTTPError as e: return e.code, e.read()
    except Exception as e: return -1, str(e).encode()

def js(raw):
    try: return json.loads(raw)
    except Exception: return {}

def start():
    subprocess.Popen([BIN, "--data-dir=" + DATA, "--port=8080", "--tls-port=8443",
                      "--tls-cert=" + CERTS + "/tls_cert.pem", "--tls-key=" + CERTS + "/tls_key.pem",
                      "--auth-token=abc123", "--workers=8"],
                     stdout=open(LOG, "ab"), stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(80):
        time.sleep(0.15)
        if req("GET", "/healthz", tok=None, tout=2)[0] == 200: return True
    return False

def stop():
    subprocess.run(["pkill", "-9", "-f", "cloudvault-server"])
    time.sleep(0.5)

def up(name, data, dirp="", stop_after=None):
    """上传；stop_after=N 表示传完前 N 块就返回（用于断电测试）"""
    h = hashlib.sha256(data).hexdigest()
    st, raw = req("POST", "/api/v1/uploads/init",
                  {"name": name, "size": len(data), "chunk_size": CHUNK, "hash": h, "dir": dirp})
    if st != 200: return False, "init %s %s" % (st, raw[:150])
    d = js(raw)
    if d.get("done"): return True, {"instant": True, "file_id": d.get("file_id"), "uid": ""}
    uid = str(d.get("upload_id")); recv = int(d.get("received_bytes") or 0)
    seq = 0; sent = 0
    while seq * CHUNK < len(data):
        off = seq * CHUNK; part = data[off:off + CHUNK]
        if off + len(part) <= recv: seq += 1; continue          # 服务端已有 -> 跳过（续传）
        if stop_after is not None and sent >= stop_after: break
        st2, rb = req("PUT", "/api/v1/uploads/%s/chunk/%d" % (uid, seq), part,
                      hdrs={"X-Chunk-SHA256": hashlib.sha256(part).hexdigest()})
        if st2 != 200: return False, "chunk %d -> %s %s" % (seq, st2, rb[:120])
        sent += 1; seq += 1
    if stop_after is not None: return True, {"uid": uid, "sent": sent}
    st3, rb = req("POST", "/api/v1/uploads/%s/complete" % uid)
    if st3 != 200: return False, "complete %s %s" % (st3, rb[:200])
    return True, {"uid": uid, "info": js(rb)}

def mkdie(dirp):
    """建云端目录（真实客户端也是先 ensureFolderPath 再上传）"""
    st, raw = req("POST", "/api/v1/dirs", {"path": dirp})
    return st in (200, 201, 409)   # 新建返回 201，已存在返回 200

def flist(dirp=""):
    st, raw = req("GET", "/api/v1/files?dir=" + dirp)
    return js(raw).get("items", []) if st == 200 else []

def rnd(n): return os.urandom(n)

# ---------------------------------------------------------------- 开跑
print("清理测试数据目录 …")
stop()
if os.path.isdir(DATA): shutil.rmtree(DATA, ignore_errors=True)
os.makedirs(DATA, exist_ok=True)
if not start(): print("服务端起不来，终止"); sys.exit(2)

sec("T1 健康检查与鉴权")
st, raw = req("GET", "/healthz", tok=None)
d = js(raw)
ck("healthz 200 + status=ok", st == 200 and d.get("status") == "ok", (st, raw[:120]))
ck("无 token 访问业务接口 -> 401", req("GET", "/api/v1/files", tok=None)[0] == 401)
ck("带 token -> 200", req("GET", "/api/v1/files")[0] == 200)

sec("T2 单块上传 + 内容校验")
small = ("hello cloudvault " * 100).encode()
ok, info = up("t2-small.txt", small)
items = flist(); hit = [x for x in items if x.get("name") == "t2-small.txt"]
ck("上传成功", ok, info)
ck("列表出现该文件", len(hit) == 1, items)
ck("服务端 hash 与本地一致", hit and hit[0].get("hash") == hashlib.sha256(small).hexdigest(), hit)

sec("T3 分块边界（chunk = 5 MiB）")
cases = [("t3-0B.bin", 0), ("t3-1B.bin", 1), ("t3-5M.bin", CHUNK),
         ("t3-5M+1.bin", CHUNK + 1), ("t3-10M+1.bin", 2 * CHUNK + 1)]
for nm, sz in cases:
    data = rnd(sz)
    ok, info = up(nm, data)
    items = flist(); hit = [x for x in items if x.get("name") == nm]
    ck("%s(%d B) 上传+hash 一致" % (nm, sz),
       ok and len(hit) == 1 and hit[0].get("hash") == hashlib.sha256(data).hexdigest()
       and int(hit[0].get("size") or 0) == sz, info)

sec("T4 秒传（同内容再传）")
same = rnd(2 * CHUNK)
ok1, _ = up("t4-first.bin", same)
ok2, info2 = up("t4-second.bin", same)
ck("第一个正常上传", ok1)
ck("第二个命中秒传(done=true)", ok2 and info2.get("instant") is True, info2)

sec("T5 断电续传（kill -9 模拟断电）")
big = rnd(6 * CHUNK)                       # 30MB / 6 块
ok, info = up("t5-resume.bin", big, stop_after=3)
ck("传到第 3 块后断电", ok and info.get("sent") == 3, info)
stop()
ck("服务端已停", req("GET", "/healthz", tok=None, tout=2)[0] != 200)
ck("服务端已重启", start())
st, raw = req("POST", "/api/v1/uploads/init",
              {"name": "t5-resume.bin", "size": len(big), "chunk_size": CHUNK,
               "hash": hashlib.sha256(big).hexdigest(), "dir": ""})
d = js(raw); recv = int(d.get("received_bytes") or 0)
ck("重启后会话仍在，received_bytes = 3 块", st == 200 and recv >= 3 * CHUNK, (st, d))
ok, info = up("t5-resume.bin", big)        # 从断点补完
items = flist(); hit = [x for x in items if x.get("name") == "t5-resume.bin"]
ck("续传后完成且 hash 一致",
   ok and hit and hit[0].get("hash") == hashlib.sha256(big).hexdigest(), info)

sec("T6 块损坏检测（篡改 .part 后合并）")
corrupt = rnd(2 * CHUNK)
h = hashlib.sha256(corrupt).hexdigest()
st, raw = req("POST", "/api/v1/uploads/init",
              {"name": "t6-corrupt.bin", "size": len(corrupt), "chunk_size": CHUNK, "hash": h, "dir": ""})
d = js(raw); uid6 = str(d.get("upload_id"))
for s in (0, 1):
    part = corrupt[s * CHUNK:(s + 1) * CHUNK]
    req("PUT", "/api/v1/uploads/%s/chunk/%d" % (uid6, s), part,
        hdrs={"X-Chunk-SHA256": hashlib.sha256(part).hexdigest()})
pdir = os.path.join(DATA, "tmp", "uploads", uid6)
parts = sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []
ck("找到临时分块文件", len(parts) >= 2, pdir + " " + str(parts))
if parts:
    tgt = os.path.join(pdir, parts[0])
    with open(tgt, "wb") as f: f.write(b"\x00" * os.path.getsize(tgt))   # 篡改第 0 块
    st, rb = req("POST", "/api/v1/uploads/%s/complete" % uid6)
    body = rb.decode("utf-8", "replace")
    # 服务端可能报 422 invalid-chunks，也可能把"哈希不符"归入 missing（都需要重传该块）
    rejected = (st == 422 and "invalid-chunks" in body) or (st == 409 and "missing" in body)
    ck("合并被拒（422 invalid-chunks 或 409 missing）", rejected, (st, body[:200]))
    part0 = corrupt[0:CHUNK]
    req("PUT", "/api/v1/uploads/%s/chunk/0" % uid6, part0,
        hdrs={"X-Chunk-SHA256": hashlib.sha256(part0).hexdigest()})
    st, rb = req("POST", "/api/v1/uploads/%s/complete" % uid6)
    items = flist(); hit = [x for x in items if x.get("name") == "t6-corrupt.bin"]
    ck("重传坏块后合并成功且 hash 一致",
       st == 200 and hit and hit[0].get("hash") == h, (st, rb[:150]))

sec("T7 批量移入回收站（文件 + 目录）")
fa = rnd(4096); fb = rnd(4096); fc = rnd(4096)
ck("建目录 bt-dir", mkdie("bt-dir"))
ua, _ = up("bt-a.bin", fa); ub, _ = up("bt-b.bin", fb); uc, _ = up("bt-c.bin", fc, dirp="bt-dir")
ids = [x.get("id") for x in flist() if x.get("name") in ("bt-a.bin", "bt-b.bin")]
ck("两个文件已就绪", len(ids) == 2, ids)
st, raw = req("POST", "/api/v1/files/trash", {"ids": ids, "dirs": ["bt-dir"]})
d = js(raw)
ck("批量删除返回 (文件2 + 目录1)", st == 200 and d.get("ok_count") == 3 and d.get("failed_count") == 0, (st, d))
left = [x.get("name") for x in flist()]
ck("列表里已消失", not ({"bt-a.bin", "bt-b.bin", "bt-dir"} & set(left)), left)

sec("T8 批量移动")
m1 = rnd(2048); m2 = rnd(2048)
ck("建目录 mv-src / mv-dst", mkdie("mv-src") and mkdie("mv-dst"))
up("mv-1.bin", m1, dirp="mv-src"); up("mv-2.bin", m2, dirp="mv-src")
src = flist("mv-src"); mids = [x.get("id") for x in src]
st, raw = req("POST", "/api/v1/files/move", {"ids": mids, "dir": "mv-dst"})
d = js(raw)
ck("批量移动 2 项成功", st == 200 and d.get("ok_count") == 2, (st, d))
ck("目标目录里确实有 2 个", len(flist("mv-dst")) == 2, flist("mv-dst"))

sec("T9 并发（20 路）")
lat = []; errs = []
def hit():
    t0 = time.time(); st, _ = req("GET", "/api/v1/storage"); dt = time.time() - t0
    lat.append(dt)
    if st != 200: errs.append(st)
ths = [threading.Thread(target=hit) for _ in range(20)]
[t.start() for t in ths]; [t.join() for t in ths]
ck("20/20 全部 200", len(errs) == 0, errs)
ck("最慢单条 < 1s", max(lat) < 1.0, "max=%.3fs" % max(lat))

print("\n---------------- 汇总 ----------------")
print("通过 %d 项，失败 %d 项" % (len(P), len(F)))
if F:
    print("失败项：")
    for x in F: print("  - " + x)
subprocess.run(["pkill", "-9", "-f", "cloudvault-server"]); time.sleep(0.3)
sys.exit(0 if not F else 1)
