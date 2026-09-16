#!/usr/bin/env bash
# CloudVault 启动期行为验收测试（--http=off）。
#
# 前置条件
# --------
# - 已构建 ./build/cloudvault-server（或用 CV_SERVER_BIN 覆盖路径），构建启用 OpenSSL（否则跳过需要 TLS 的检查）。
# - 只能在 Linux/Debian 上运行。需要 openssl（用于现场生成自签证书）。
#
# 覆盖
# ----
#   A) --http=off 且未配 --tls-port        → 必须非零退出，且有明确错误信息；
#   B) --http=off + 正确 --tls-port/--tls-cert/--tls-key
#        → 必须正常运行；“明文 HTTP 已关闭 / 仅保留 HTTPS”类提示必须出现在 TLS 监听成功之后；
#   C) --http=off + --tls-port 但缺 --tls-cert/--tls-key → 必须非零退出（不静默降级）。
#
# 用法
# ----
#   CV_SERVER_BIN=./build/cloudvault-server bash http_off_test.sh
#
# 退出码：全部通过 0；存在 FAIL 1；找不到可执行文件 2。
set -uo pipefail

SRV=${CV_SERVER_BIN:-${CV_SERVER:-./build/cloudvault-server}}
DIR=${CV_HTTP_OFF_DIR:-/tmp/cv-http-off}
TLS_PORT=${CV_HTTP_OFF_TLS_PORT:-8443}

pass=0
fail=0
skip=0
ok()   { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }
skipped() { printf 'SKIP  %s\n' "$1"; skip=$((skip + 1)); }

if [ ! -x "$SRV" ]; then
  echo "错误：找不到可执行服务端 '$SRV'（用 CV_SERVER_BIN=... 指定）" >&2
  exit 2
fi

PID=""
stop_server() {
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    PID=""
  fi
}
cleanup() {
  stop_server
  rm -rf "$DIR"
}
trap 'cleanup' EXIT

rm -rf "$DIR"
mkdir -p "$DIR"

# ---------------- A) --http=off 且未配 --tls-port ----------------
echo "== A) --http=off 且未配 --tls-port（应报错退出）=="
OUTA=$( "$SRV" --data-dir "$DIR" --http=off 2>&1 )
RCA=$?
if [ "$RCA" -ne 0 ]; then
  ok "--http=off 无 TLS → 非零退出（rc=$RCA）"
else
  bad "--http=off 无 TLS 却成功启动（rc=0）"
fi
if printf '%s' "$OUTA" | grep -qE '配置冲突|tls-port|无任何监听|仅保留 HTTPS'; then
  ok "错误信息包含明确的 --http=off/TLS 提示"
else
  bad "未看到明确的错误提示（输出：$(printf '%s' "$OUTA" | tr '\n' ' ' | head -c 160)）"
fi

# ---------------- C) --http=off + --tls-port 但缺证书/私钥 ----------------
echo "== C) --http=off + --tls-port 但缺证书/私钥（应报错退出）=="
OUTC=$( "$SRV" --data-dir "$DIR" --http=off --tls-port "$TLS_PORT" 2>&1 )
RCC=$?
if [ "$RCC" -ne 0 ]; then
  ok "缺 --tls-cert/--tls-key → 非零退出（rc=$RCC）"
else
  bad "缺证书/私钥却成功启动（rc=0）"
fi
if printf '%s' "$OUTC" | grep -qE 'tls-cert|tls-key|TLS|证书|OpenSSL'; then
  ok "错误信息包含证书/私钥或 TLS 提示"
else
  bad "缺证书的错误提示不明确（输出：$(printf '%s' "$OUTC" | tr '\n' ' ' | head -c 160)）"
fi

# ---------------- B) --http=off + 正确 TLS（应正常启动且顺序正确）----------------
echo "== B) --http=off + 正确 TLS（应正常启动；提示顺序须在 TLS 成功之后）=="
if ! command -v openssl >/dev/null 2>&1; then
  skipped "未找到 openssl，跳过 B（无法生成证书）"
else
  openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=t" \
    -keyout "$DIR/k.pem" -out "$DIR/c.pem" >/dev/null 2>&1
  if [ ! -s "$DIR/c.pem" ] || [ ! -s "$DIR/k.pem" ]; then
    skipped "证书生成失败，跳过 B"
  else
    LOG="$DIR/phaseB.log"
    "$SRV" --data-dir "$DIR" --http=off --tls-port "$TLS_PORT" \
           --tls-cert "$DIR/c.pem" --tls-key "$DIR/k.pem" >"$LOG" 2>&1 &
    PID=$!
    sleep 1.5
    if ! kill -0 "$PID" 2>/dev/null; then
      if grep -q "未编译 TLS" "$LOG"; then
        skipped "二进制未编译 TLS 支持，跳过 B（见 $LOG）"
      else
        bad "B 服务端未能启动（见 $LOG）"
      fi
    else
      ok "B 服务端正常启动（--http=off + TLS）"
      TLS_LINE=$(grep -nE 'HTTPS 监听|TLS 监听|HTTPS.*监听' "$LOG" | head -n1 | cut -d: -f1)
      OFF_LINE=$(grep -nE '仅保留 HTTPS|明文 HTTP 已关闭|明文 HTTP 已按|--http=off' "$LOG" | head -n1 | cut -d: -f1)
      if [ -z "$TLS_LINE" ]; then
        bad "日志中未找到 TLS 监听成功标记（见 $LOG）"
      elif [ -z "$OFF_LINE" ]; then
        ok "日志顺序 OK（TLS 成功，且无提前的 --http=off 提示）"
      elif [ "$OFF_LINE" -gt "$TLS_LINE" ]; then
        ok "日志顺序正确：--http=off 提示（行 $OFF_LINE）在 TLS 成功（行 $TLS_LINE）之后"
      else
        bad "日志顺序错误：--http=off 提示（行 $OFF_LINE）早于 TLS 成功（行 $TLS_LINE）"
      fi
      stop_server
    fi
  fi
fi

echo "-----------------------------------------"
printf 'TOTAL: %d PASS, %d FAIL, %d SKIP\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ] || exit 1
