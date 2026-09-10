#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 云匣服务端冒烟测试（依赖：curl、sha256sum、diff、head、base64）
#
# 用法：
#   1) 启动服务端：./build/cloudvault-server --data-dir=/tmp/cvdata --port=8080
#   2) 运行：      ./testdata/smoke.sh
#      换端口：    BASE=http://127.0.0.1:9090 ./testdata/smoke.sh
#
# 全部通过输出 "冒烟测试全部通过"，任一失败以非零码退出。
# ---------------------------------------------------------------------------
set -u

BASE="${BASE:-http://127.0.0.1:8080}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "[通过] $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "[失败] $1"; }
check() { # check <说明> <条件命令...>
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}

# 从 JSON 响应取字段（不依赖 jq）
json_num() { printf '%s' "$1" | grep -o "\"$2\":[0-9]*" | head -1 | cut -d: -f2; }
json_bool() { printf '%s' "$1" | grep -o "\"$2\":\(true\|false\)" | head -1 | cut -d: -f2; }
json_str() { printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | cut -d'"' -f4; }

upload() { # upload <本地文件> <URL编码后的文件名> -> 输出响应
  curl -fsS -X POST --data-binary @"$1" -H "X-CV-Name: $2" "$BASE/api/v1/files"
}

echo "== 0. 健康检查 =="
HEALTH="$(curl -fsS "$BASE/healthz" 2>/dev/null)" || HEALTH=""
check "/healthz 返回 ok" test "$(json_str "$HEALTH" status)" = "ok"

echo "== 1. 上传 hello.txt 并核对哈希 =="
R1="$(upload "$DIR/hello.txt" "hello.txt")" || R1=""
ID1="$(json_num "$R1" id)"
H_LOCAL="$(sha256sum "$DIR/hello.txt" | cut -d' ' -f1)"
H_RESP="$(json_str "$R1" hash)"
check "hello.txt 上传成功（有 id）" test -n "$ID1"
check "服务端返回的 hash 与本地一致" test "$H_RESP" = "$H_LOCAL"

echo "== 2. 同内容再传一次 —— 秒传命中 =="
R2="$(upload "$DIR/hello.txt" "hello-copy.txt")" || R2=""
check "第二次上传 instant=true" test "$(json_bool "$R2" instant)" = "true"

echo "== 3. 中文文件名 + 下载回读比对 =="
R3="$(upload "$DIR/notes.txt" "%E6%B5%8B%E8%AF%95%E7%AC%94%E8%AE%B0.txt")" || R3=""
ID3="$(json_num "$R3" id)"
NAME3="$(json_str "$R3" name)"
curl -fsS -o "$TMP/dl_notes.txt" "$BASE/api/v1/files/$ID3/content"
check "中文名正确解码（测试笔记.txt）" test "$NAME3" = "测试笔记.txt"
check "下载内容与原文件一致" diff -q "$DIR/notes.txt" "$TMP/dl_notes.txt"

echo "== 4. 重复内容文件（dup-a / dup-b）=="
RA="$(upload "$DIR/dup-a.txt" "dup-a.txt")" || RA=""
RB="$(upload "$DIR/dup-b.txt" "dup-b.txt")" || RB=""
check "dup-b 秒传命中（内容同 dup-a）" test "$(json_bool "$RB" instant)" = "true"
check "两者 hash 相同" test "$(json_str "$RA" hash)" = "$(json_str "$RB" hash)"

echo "== 5. 空文件边界 =="
RE="$(upload "$DIR/empty.txt" "empty.txt")" || RE=""
check "空文件上传成功（size=0）" test "$(json_num "$RE" size)" = "0"
curl -fsS -o "$TMP/dl_empty.txt" "$BASE/api/v1/files/$(json_num "$RE" id)/content"
check "空文件下载仍为空" test ! -s "$TMP/dl_empty.txt"

echo "== 6. 大文件跨分块（约 6MB，默认 5MB 分块应为 2 块）= ="
BIG="$TMP/big.txt"
head -c 6291456 /dev/urandom | base64 | tr -d '\n' | head -c 6291456 > "$BIG"
RBIG="$(upload "$BIG" "big.txt")" || RBIG=""
CHUNKS="$(json_num "$RBIG" chunks)"
check "大文件上传成功" test -n "$(json_num "$RBIG" id)"
check "分块数 >= 2（实际 $CHUNKS）" test "$CHUNKS" -ge 2
curl -fsS -o "$TMP/dl_big.txt" "$BASE/api/v1/files/$(json_num "$RBIG" id)/content"
check "大文件下载哈希一致" test \
  "$(sha256sum "$BIG" | cut -d' ' -f1)" = "$(sha256sum "$TMP/dl_big.txt" | cut -d' ' -f1)"

echo "== 7. 列表 =="
LIST="$(curl -fsS "$BASE/api/v1/files")" || LIST=""
TOTAL="$(json_num "$LIST" total)"
check "列表 total >= 5（实际 $TOTAL）" test "$TOTAL" -ge 5

echo
echo "通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then
  echo "冒烟测试全部通过"
  exit 0
fi
exit 1
