#!/usr/bin/env bash
# CloudVault 静态数据加密（AES-256-GCM）往返 + 落盘格式验收测试。
#
# 前置条件
# --------
# - 已构建 ./cloudvault-server（或用 CV_SERVER 指定可执行文件路径），
#   且构建启用了 OpenSSL（CMake 找到 libssl-dev 时自动定义 CV_HAVE_OPENSSL）。
# - 只能在 Linux/Debian 上运行（POSIX socket + SQLite3；Windows 无法运行服务端）。
# - 需要 python3 无关；需要 curl、sha256sum、od、dd、find、cmp。
#
# 设计要点（与实现一致，见 server/README.md §4.5）
# -------------------------------------------------
# 启用加密后 blob 落盘：文件名带 .enc 后缀，内容为 "CVB1" + nonce(12) + tag(16) + ciphertext；
# 读取时先找 <hash>.enc，找不到再回退 <hash>（旧明文库共存）。故本脚本断言：
#   - blobs 目录下存在 *.enc，且其前 4 字节为 CVB1；
#   - blobs 目录下不存在任何【无 .enc 后缀】的文件（即无明文副本残留）。
#
# 用法
# ----
#   CV_SERVER=./cloudvault-server bash encryption_roundtrip.sh
#
# 退出码：全部通过 0；存在 FAIL 1。
set -u

SRV=${CV_SERVER_BIN:-${CV_SERVER:-./build/cloudvault-server}}
DD=${CV_DATA_DIR:-/tmp/cv-enc-test}
KEY=${CV_KEY_FILE:-/tmp/cv-data.key}
BADKEY=${CV_BAD_KEY_FILE:-/tmp/cv-bad.key}
PORT=${CV_PORT:-8080}
T=${CV_TOKEN:-s3cr3t}
B="http://127.0.0.1:$PORT"

pass=0
fail=0
ok()  { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }

# 以给定参数运行服务端；若在极短时间内退出则视为“启动失败”（期望行为）。
expect_fail_start() {
  local desc=$1
  shift
  if "$SRV" "$@" >/dev/null 2>&1; then
    bad "$desc（进程意外成功启动，应报错退出）"
  else
    ok "$desc"
  fi
}

if [ ! -x "$SRV" ]; then
  echo "错误：找不到可执行服务端 '$SRV'（用 CV_SERVER=... 指定）" >&2
  exit 2
fi

rm -rf "$DD"
mkdir -p "$DD"
# 生成 64 位十六进制（32 字节）密钥
head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$KEY"
printf 'short' > "$BADKEY"   # 长度非法（既非 64 hex 也非 32 字节）

echo "== 1) 非法加密配置必须启动失败 =="
expect_fail_start "缺密钥文件应启动失败" --data-dir "$DD" --port 0 --data-key /tmp/cv-nonexistent.key
expect_fail_start "密钥长度非法应启动失败" --data-dir "$DD" --port 0 --data-key "$BADKEY"

echo "== 2) 启动加密服务端 =="
"$SRV" --data-dir "$DD" --port "$PORT" --auth-token "$T" --data-key "$KEY" &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null' EXIT
sleep 1
if ! kill -0 "$SRV_PID" 2>/dev/null; then
  bad "服务端未能启动（端口占用或配置错误？）"
  printf 'TOTAL: %d PASS, %d FAIL\n' "$pass" "$fail"
  exit 1
fi
ok "服务端已启动（pid=$SRV_PID）"

echo "== 3) 上传 / 下载往返一致 =="
head -c 300000 /dev/urandom > /tmp/cv-local.bin
UP=$(curl -s --max-time 10 -H "Authorization: Bearer $T" -H 'X-CV-Name: roundtrip.bin' \
        --data-binary @/tmp/cv-local.bin "$B/api/v1/files")
ID=$(printf '%s' "$UP" | sed -n 's/.*"id":\([0-9]*\).*/\1/p')
if [ -n "$ID" ]; then ok "上传取得 id=$ID"; else bad "上传未取得 id（响应：$UP）"; fi

curl -s --max-time 10 -H "Authorization: Bearer $T" -o /tmp/cv-down.bin "$B/api/v1/files/$ID/content"
if cmp -s /tmp/cv-local.bin /tmp/cv-down.bin; then
  ok "下载内容与原文一致（往返 cmp 通过）"
else
  bad "下载内容与原文不一致"
fi

echo "== 4) 落盘为密文（.enc 后缀）且无明文残留 =="
if [ -n "$(find "$DD/blobs" -type f -name '*.enc' 2>/dev/null)" ]; then
  ok "存在 .enc 加密 blob"
else
  bad "未找到 .enc 加密 blob（加密未生效或命名不符）"
fi
ENC=$(find "$DD/blobs" -type f -name '*.enc' 2>/dev/null | head -n1)
if [ -n "$ENC" ]; then
  MAGIC=$(head -c4 "$ENC")
  if [ "$MAGIC" = "CVB1" ]; then ok "加密 blob 前 4 字节为 CVB1"; else bad "加密 blob magic 非 CVB1（实得 '$MAGIC'）"; fi
fi
PLAIN=$(find "$DD/blobs" -type f ! -name '*.enc' 2>/dev/null)
if [ -z "$PLAIN" ]; then
  ok "blobs 目录内无未加密（无 .enc 后缀）明文 blob"
else
  bad "发现未加密明文 blob：$PLAIN"
fi

echo "== 5) 内容寻址键仍为明文 sha256（去重语义不变）=="
H=$(sha256sum /tmp/cv-local.bin | cut -d' ' -f1)
if printf '%s' "$UP" | grep -q "$H"; then
  ok "上传响应 hash == 明文 sha256"
else
  bad "上传响应未含明文 sha256（响应：$UP）"
fi
UP2=$(curl -s --max-time 10 -H "Authorization: Bearer $T" -H 'X-CV-Name: roundtrip-copy.bin' \
        --data-binary @/tmp/cv-local.bin "$B/api/v1/files")
if printf '%s' "$UP2" | grep -q '"instant":true'; then
  ok "重复内容命中秒传（instant=true）"
else
  bad "重复内容未命中秒传（响应：$UP2）"
fi

echo "== 6) Range 下载（加密下走分块拼装 + 解密）=="
curl -s --max-time 10 -H "Authorization: Bearer $T" -H 'Range: bytes=100-199' \
     -o /tmp/cv-range.bin "$B/api/v1/files/$ID/content"
dd if=/tmp/cv-local.bin bs=1 skip=100 count=100 of=/tmp/cv-range-expect.bin status=none 2>/dev/null
if cmp -s /tmp/cv-range.bin /tmp/cv-range-expect.bin; then
  ok "Range 100-199 往返一致"
else
  bad "Range 往返不一致（可能未按分块拼装+解密处理）"
fi

echo "-----------------------------------------"
printf 'TOTAL: %d PASS, %d FAIL\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
