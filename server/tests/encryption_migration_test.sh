#!/usr/bin/env bash
# CloudVault 静态加密「迁移 / 混合库」验收测试（.enc 与旧明文库共存）。
#
# 前置条件
# --------
# - 已构建 ./build/cloudvault-server（或用 CV_SERVER_BIN 指定），构建启用 OpenSSL。
# - 只能在 Linux/Debian 上运行（POSIX socket + SQLite3）。
# - 需要：curl、cmp、find、od、sed、seq。
#
# 设计前提（与实现一致）
# ---------------------
# 启用 --data-key 后，新 blob 落盘为 <hash>.enc（内容 CVB1+nonce+tag+ciphertext）；
# 读取时先找 <hash>.enc，找不到再回退 <hash>（旧明文），不使用内容嗅探。
#
# 三个阶段
# --------
#   阶段1：不带 --data-key 启动 → 上传 fileA（明文 blob）；
#   阶段2：带 --data-key 重启 → 旧 fileA 仍能下载且一致（回退明文）；新上传 fileB → *.enc；
#          旧明文 blob 仍保留；
#   阶段3：不带 --data-key 重启 → 无密钥读 fileB(.enc) 必须是干净失败（5xx，且不泄漏明文）。
#
# 用法
# ----
#   CV_SERVER_BIN=./build/cloudvault-server bash encryption_migration_test.sh
#
# 退出码：全部通过 0；存在 FAIL 1；找不到可执行文件 2。
set -uo pipefail

SRV=${CV_SERVER_BIN:-${CV_SERVER:-./build/cloudvault-server}}
DD=${CV_MIG_DATA_DIR:-/tmp/cv-mig-test}
KEY=${CV_MIG_KEY_FILE:-/tmp/cv-mig-data.key}
PORT=${CV_MIG_PORT:-8082}
T=${CV_MIG_TOKEN:-s3cr3t}
B="http://127.0.0.1:$PORT"

pass=0
fail=0
ok()   { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }
note() { printf 'NOTE  %s\n' "$1"; }

if [ ! -x "$SRV" ]; then
  echo "错误：找不到可执行服务端 '$SRV'（用 CV_SERVER_BIN=... 指定）" >&2
  exit 2
fi

PID=""
LOG=""
stop_server() {
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    PID=""
  fi
}
trap 'stop_server' EXIT

# start_server <data-key 或 空>
start_server() {
  local key=$1 i
  if [ -n "$key" ]; then
    "$SRV" --data-dir "$DD" --port "$PORT" --auth-token "$T" --data-key "$key" >>"$LOG" 2>&1 &
  else
    "$SRV" --data-dir "$DD" --port "$PORT" --auth-token "$T" >>"$LOG" 2>&1 &
  fi
  PID=$!
  for i in $(seq 1 50); do
    if curl -s -o /dev/null --max-time 1 "$B/healthz"; then return 0; fi
    if ! kill -0 "$PID" 2>/dev/null; then return 1; fi
    sleep 0.1
  done
  return 1
}

upload() {  # <file> <name>  → 打印上传响应 JSON
  curl -s --max-time 15 -H "Authorization: Bearer $T" -H "X-CV-Name: $2" \
       --data-binary @"$1" "$B/api/v1/files"
}
id_of() { printf '%s' "$1" | sed -n 's/.*"id":\([0-9]*\).*/\1/p'; }

rm -rf "$DD"
mkdir -p "$DD"

# --------------------------- 阶段 1：明文库 ---------------------------
echo "== 阶段 1：明文库（不带 --data-key）=="
LOG="$DD/phase1.log"
head -c 100000 /dev/urandom > /tmp/cv-mig-a.bin
PLAIN_A=""
ID_A=""
if ! start_server ""; then
  bad "阶段1 服务端未启动（见 $LOG）"
else
  UP_A=$(upload /tmp/cv-mig-a.bin fileA.bin)
  ID_A=$(id_of "$UP_A")
  if [ -n "$ID_A" ]; then ok "阶段1 上传 fileA id=$ID_A"; else bad "阶段1 上传失败：$UP_A"; fi
  PLAIN_A=$(find "$DD/blobs" -type f ! -name '*.enc' 2>/dev/null | head -n1)
  if [ -n "$PLAIN_A" ]; then ok "阶段1 存在明文 blob（无 .enc）：$PLAIN_A"; else bad "阶段1 未找到明文 blob"; fi
  stop_server
fi

# --------------------------- 阶段 2：加密开启 ---------------------------
echo "== 阶段 2：加密开启（带 --data-key）=="
LOG="$DD/phase2.log"
head -c32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$KEY"
ID_B=""
if ! start_server "$KEY"; then
  bad "阶段2 服务端未启动（见 $LOG）"
else
  if [ -n "$ID_A" ]; then
    curl -s --max-time 15 -H "Authorization: Bearer $T" -o /tmp/cv-mig-a.down \
         "$B/api/v1/files/$ID_A/content"
    if cmp -s /tmp/cv-mig-a.bin /tmp/cv-mig-a.down; then
      ok "阶段2 旧明文文件仍可下载且一致（先 .enc 未命中 → 回退明文）"
    else
      bad "阶段2 旧明文文件下载不一致"
    fi
  else
    bad "阶段2 无法验证回退（阶段1 未取得 id）"
  fi

  head -c 100000 /dev/urandom > /tmp/cv-mig-b.bin
  UP_B=$(upload /tmp/cv-mig-b.bin fileB.bin)
  ID_B=$(id_of "$UP_B")
  if [ -n "$ID_B" ]; then ok "阶段2 上传 fileB id=$ID_B"; else bad "阶段2 上传失败：$UP_B"; fi

  if [ -n "$(find "$DD/blobs" -type f -name '*.enc' 2>/dev/null)" ]; then
    ok "阶段2 新 blob 为 *.enc（加密落盘生效）"
  else
    bad "阶段2 未找到 *.enc（.enc 后缀设计未落地？）"
  fi

  if [ -n "$PLAIN_A" ] && [ -f "$PLAIN_A" ]; then
    ok "阶段2 旧明文 blob 仍保留（未被迁移过程删除）"
  else
    bad "阶段2 旧明文 blob 丢失：$PLAIN_A"
  fi
  stop_server
fi

# --------------------------- 阶段 3：无密钥读密文 ---------------------------
echo "== 阶段 3：无密钥读密文（不带 --data-key）=="
LOG="$DD/phase3.log"
if ! start_server ""; then
  bad "阶段3 服务端未启动（见 $LOG）"
else
  if [ -n "$ID_B" ]; then
    code=$(curl -s --max-time 15 -o /tmp/cv-mig-b.out -w '%{http_code}' \
           -H "Authorization: Bearer $T" "$B/api/v1/files/$ID_B/content" || echo "000")
    note "阶段3 无密钥下载 fileB(.enc) → HTTP $code"
    if [ "$code" = "000" ] || [ "${code:0:1}" = "5" ]; then
      ok "阶段3 无密钥读密文为干净失败（5xx / 连接错误，非 2xx）"
    else
      bad "阶段3 无密钥读密文返回 $code（期望 5xx）"
    fi
    if [ -s /tmp/cv-mig-b.out ] && cmp -s /tmp/cv-mig-b.bin /tmp/cv-mig-b.out; then
      bad "阶段3 泄漏了明文（响应体与原文逐字节一致）"
    else
      ok "阶段3 未泄漏明文（响应体不是原文）"
    fi
  else
    bad "阶段3 无法验证（阶段2 未取得 id）"
  fi
  stop_server
fi

echo "-----------------------------------------"
printf 'TOTAL: %d PASS, %d FAIL\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
