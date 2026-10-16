#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 云匣服务端「分块上传 + 断点续传」端到端冒烟测试
#
# 依赖：curl、sha256sum、dd、stat（仅需 Linux/类 Unix；服务端已构建并运行）。
# 不依赖 jq，仅用 grep/sed 解析 JSON。
#
# 用法：
#   1) 启动服务端：./build/cloudvault-server --data-dir=/tmp/cvdata --port=8080
#   2) 运行：      ./testdata/smoke_chunked.sh
#      换端口：    BASE=http://127.0.0.1:9090 ./testdata/smoke_chunked.sh
#
# 覆盖点：
#   - init → 逐块 PUT（带 X-Chunk-SHA256）→ complete → 下载回读比对 sha256
#   - 断点续传：新内容只传前一半分块，再 init 应返回同一 upload_id 且 uploaded 含已传 seq，
#     传剩余分块后 complete 成功，哈希一致
#   - 秒传：同内容第二次 init 应返回 done:true
#   - 空文件（size=0）走通 init → complete → 下载为空
#   - Range 下载（206）比对首段字节
#   - 负例：缺分块 → 409 {missing}；整文件哈希不符 → 422 {invalid}
#   - 取消会话：DELETE → 204，随后 GET → 404
#
# 全部通过输出 "冒烟测试全部通过"，任一失败以非零码退出。
# 注意：Windows 下 git 不保留执行位，请显式用 bash 运行此脚本。
#
# 修订记录（QA 严过关，独立验证时修正）：
#   [FIX-1] 二进制响应体不能经 shell 变量中转：`HTTP_BODY="$(...)"` / `echo "$HTTP_BODY" > f`
#           会丢 NUL 字节并增删行尾换行，导致随机二进制回读的 sha256 比较必然失败。
#           下载 / Range 一律改用 `curl -o <file> -w '%{http_code}'` 直接落盘。
#   [FIX-2] 断点续传用例必须使用「内容库中尚不存在」的新内容。若沿用步骤 1 已 complete 的
#           同一内容，同 hash 的 init 会先命中秒传（done:true, upload_id=0），永远走不到
#           断点复用分支，断言 `upload_id != 0` 必失败。
#   [FIX-3] 补上契约里最关键的 409 {missing} / 422 {invalid} 负例覆盖。
# ---------------------------------------------------------------------------
set -u

BASE="${BASE:-http://127.0.0.1:8080}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
HTTP_CODE=""
HTTP_BODY=""

ok()   { PASS=$((PASS + 1)); echo "[通过] $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "[失败] $1"; }
# check <说明> <命令...>：命令返回 0 记为通过
check() {
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}

# ---- HTTP 辅助 ----
# 文本（JSON）响应用变量中转；二进制响应用 fetch_bin 直接落盘（见 FIX-1）。
http_get() {
  local out
  out="$(curl -fsS -w '\n%{http_code}' "$1" 2>/dev/null)"
  HTTP_CODE="$(printf '%s' "$out" | tail -1)"
  HTTP_BODY="$(printf '%s' "$out" | sed '$d')"
}
http_json() {  # method url [bodyfile] [header ...]
  local m="$1" u="$2" bf="${3:-}"; shift 3
  local args=()
  local h
  for h in "$@"; do args+=(-H "$h"); done
  local out
  if [ -n "$bf" ]; then
    out="$(curl -fsS -w '\n%{http_code}' -X "$m" "${args[@]}" --data-binary "@$bf" "$u" 2>/dev/null)"
  else
    out="$(curl -fsS -w '\n%{http_code}' -X "$m" "${args[@]}" "$u" 2>/dev/null)"
  fi
  HTTP_CODE="$(printf '%s' "$out" | tail -1)"
  HTTP_BODY="$(printf '%s' "$out" | sed '$d')"
}
# fetch_bin <url> <outfile> [curl 额外参数...]：把二进制响应直接写入文件，回显 HTTP 码
fetch_bin() {
  local url="$1" out="$2"; shift 2
  curl -sS -o "$out" -w '%{http_code}' "$@" "$url" 2>/dev/null
}

# ---- JSON 解析（无 jq）----
get_num()     { printf '%s' "$1" | grep -o "\"$2\":[-0-9]*" | head -1 | sed "s/\"$2\"://"; }
get_str()     { printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | sed "s/\"$2\":\"//;s/\"$//"; }
get_bool()    { printf '%s' "$1" | grep -o "\"$2\":\(true\|false\)" | head -1 | sed "s/\"$2\"://"; }
get_uploaded(){ printf '%s' "$1" | grep -o '"uploaded":\[[^]]*\]' | head -1 | sed 's/"uploaded"://'; }
get_missing() { printf '%s' "$1" | grep -o '"missing":\[[^]]*\]' | head -1 | sed 's/"missing"://'; }
get_invalid() { printf '%s' "$1" | grep -o '"invalid":\[[^]]*\]' | head -1 | sed 's/"invalid"://'; }

# 上传 [start, end) 区间的分块；file 为原始文件，chunk 为分块大小
upload_range() {
  local file="$1" upid="$2" chunk="$3" start="$4" end="$5"
  local seq
  for seq in $(seq "$start" $((end - 1))); do
    dd if="$file" of="$TMP/part" bs="$chunk" skip="$seq" count=1 status=none 2>/dev/null
    local chk
    chk="$(sha256sum "$TMP/part" | cut -d' ' -f1)"
    http_json PUT "$BASE/api/v1/uploads/$upid/chunk/$seq" "$TMP/part" \
      "X-Chunk-SHA256: $chk"
    if [ "$HTTP_CODE" != "200" ]; then return 1; fi
  done
  return 0
}

echo "== 0. 健康检查 =="
http_get "$BASE/healthz"
check "/healthz 返回 200 且 status=ok" \
  test "$HTTP_CODE" = "200" && test "$(get_str "$HTTP_BODY" status)" = "ok"

# ---------------------------------------------------------------------------
echo "== 1. init → 逐块 PUT → complete → 下载回读比对 =="
BIG="$TMP/big.bin"
head -c 1000000 /dev/urandom > "$BIG"
CHUNK=300000
SIZE=$(stat -c%s "$BIG")
TOTAL=$(( (SIZE + CHUNK - 1) / CHUNK ))
BIG_HASH="$(sha256sum "$BIG" | cut -d' ' -f1)"

# init
printf '{"name":"big.bin","size":%s,"chunk_size":%s,"hash":"%s"}' "$SIZE" "$CHUNK" "$BIG_HASH" > "$TMP/init.json"
http_json POST "$BASE/api/v1/uploads/init" "$TMP/init.json"
UPID="$(get_num "$HTTP_BODY" upload_id)"
check "init 返回 upload_id" test -n "$UPID" && test "$UPID" != "0"
check "init 返回 total 推算一致 (uploaded 初始为空)" \
  test "$(get_uploaded "$HTTP_BODY")" = "[]"

# 逐块上传
check "逐块 PUT 全部成功" upload_range "$BIG" "$UPID" "$CHUNK" 0 "$TOTAL"

# complete
http_json POST "$BASE/api/v1/uploads/$UPID/complete"
FILE_ID="$(get_num "$HTTP_BODY" file_id)"
check "complete 返回 200 且 file_id" test "$HTTP_CODE" = "200" && test -n "$FILE_ID"
check "complete 返回的 hash 与本地一致" test "$(get_str "$HTTP_BODY" hash)" = "$BIG_HASH"

# 下载回读（FIX-1：二进制直接落盘）
DL_CODE="$(fetch_bin "$BASE/api/v1/files/$FILE_ID/content" "$TMP/dl.bin")"
check "下载返回 200" test "$DL_CODE" = "200"
check "下载内容 sha256 与本地一致" \
  test "$(sha256sum "$TMP/dl.bin" | cut -d' ' -f1)" = "$BIG_HASH"

# ---------------------------------------------------------------------------
echo "== 2. 断点续传：新内容只传前一半 → 再 init 应复用会话 → 传剩余 → complete =="
# FIX-2：必须用内容库中尚不存在的新内容，否则同 hash 的 init 会先命中秒传而短路。
RES="$TMP/resume.bin"
head -c 1200000 /dev/urandom > "$RES"
RCHUNK=300000
RSIZE=$(stat -c%s "$RES")
RTOTAL=$(( (RSIZE + RCHUNK - 1) / RCHUNK ))
RHASH="$(sha256sum "$RES" | cut -d' ' -f1)"
printf '{"name":"resume.bin","size":%s,"chunk_size":%s,"hash":"%s"}' "$RSIZE" "$RCHUNK" "$RHASH" > "$TMP/initr.json"

http_json POST "$BASE/api/v1/uploads/init" "$TMP/initr.json"
RUPID="$(get_num "$HTTP_BODY" upload_id)"
check "续传 init 新建会话 upload_id" test -n "$RUPID" && test "$RUPID" != "0"
RHALF=$(( RTOTAL / 2 ))            # 前一半分块
check "上传前一半分块成功" upload_range "$RES" "$RUPID" "$RCHUNK" 0 "$RHALF"

# 再 init（同 hash/size/chunk）→ 应复用同一 upload_id，uploaded 含已传 seq
http_json POST "$BASE/api/v1/uploads/init" "$TMP/initr.json"
REUPID="$(get_num "$HTTP_BODY" upload_id)"
check "再 init 复用同一 upload_id" test "$REUPID" = "$RUPID"
REXPECTED="[$(seq -s, 0 $((RHALF - 1)))]"
check "再 init 的 uploaded 含已传分块" test "$(get_uploaded "$HTTP_BODY")" = "$REXPECTED"

# 传剩余分块并 complete
check "上传剩余分块成功" upload_range "$RES" "$RUPID" "$RCHUNK" "$RHALF" "$RTOTAL"
http_json POST "$BASE/api/v1/uploads/$RUPID/complete"
RFILE="$(get_num "$HTTP_BODY" file_id)"
check "续传 complete 成功且 hash 一致" \
  test "$HTTP_CODE" = "200" && test "$(get_str "$HTTP_BODY" hash)" = "$RHASH"
DL2_CODE="$(fetch_bin "$BASE/api/v1/files/$RFILE/content" "$TMP/dl2.bin")"
check "续传下载返回 200" test "$DL2_CODE" = "200"
check "续传下载 sha256 一致" \
  test "$(sha256sum "$TMP/dl2.bin" | cut -d' ' -f1)" = "$RHASH"

# ---------------------------------------------------------------------------
echo "== 3. 秒传：同内容再次 init 应返回 done:true =="
http_json POST "$BASE/api/v1/uploads/init" "$TMP/init.json"
check "同内容 init 返回 done:true" test "$(get_bool "$HTTP_BODY" done)" = "true"
check "秒传返回的 file_id 与首次一致" test "$(get_num "$HTTP_BODY" file_id)" = "$FILE_ID"

# ---------------------------------------------------------------------------
echo "== 4. 空文件（size=0）：init → complete → 下载为空 =="
EMPTY_HASH="$(sha256sum /dev/null | cut -d' ' -f1)"
printf '{"name":"empty.bin","size":0,"chunk_size":%s,"hash":"%s"}' "$CHUNK" "$EMPTY_HASH" > "$TMP/inite.json"
http_json POST "$BASE/api/v1/uploads/init" "$TMP/inite.json"
EUPID="$(get_num "$HTTP_BODY" upload_id)"
check "空文件 init 返回 upload_id" test -n "$EUPID" && test "$EUPID" != "0"
http_json POST "$BASE/api/v1/uploads/$EUPID/complete"
EFILE="$(get_num "$HTTP_BODY" file_id)"
check "空文件 complete 成功且 hash 为 sha256(\"\")" \
  test "$HTTP_CODE" = "200" && test "$(get_str "$HTTP_BODY" hash)" = "$EMPTY_HASH"
EMPTY_DL_CODE="$(fetch_bin "$BASE/api/v1/files/$EFILE/content" "$TMP/dle.bin")"
check "空文件下载返回 200" test "$EMPTY_DL_CODE" = "200"
check "空文件下载内容为空" test ! -s "$TMP/dle.bin"

# ---------------------------------------------------------------------------
echo "== 5. Range 下载（206）比对首段 =="
RNG_CODE="$(fetch_bin "$BASE/api/v1/files/$FILE_ID/content" "$TMP/rng100" -H 'Range: bytes=0-99')"
check "Range 下载返回 206" test "$RNG_CODE" = "206"
head -c 100 "$BIG" > "$TMP/head100"
check "Range 首段与本地前 100 字节一致" \
  test "$(sha256sum "$TMP/rng100" | cut -d' ' -f1)" = "$(sha256sum "$TMP/head100" | cut -d' ' -f1)"

# ---------------------------------------------------------------------------
echo "== 6. 负例：缺分块 409 {missing} / 整文件哈希不符 422 {invalid} =="
WRONG_HASH="0000000000000000000000000000000000000000000000000000000000000000"
# 409：不带 hash，init 后不上传任何分块直接 complete
printf '{"name":"miss.bin","size":%s,"chunk_size":%s,"hash":""}' "$SIZE" "$CHUNK" > "$TMP/initm.json"
http_json POST "$BASE/api/v1/uploads/init" "$TMP/initm.json"
MUPID="$(get_num "$HTTP_BODY" upload_id)"
check "缺分块 init 新建会话" test -n "$MUPID" && test "$MUPID" != "0"
http_json POST "$BASE/api/v1/uploads/$MUPID/complete"
check "缺分块 complete 返回 409" test "$HTTP_CODE" = "409"
check "409 响应含非空 missing" test -n "$(get_missing "$HTTP_BODY")"
# 422：init 给出格式合法但与内容不符的 hash，全部传完再 complete
printf '{"name":"bad.bin","size":%s,"chunk_size":%s,"hash":"%s"}' "$SIZE" "$CHUNK" "$WRONG_HASH" > "$TMP/initb.json"
http_json POST "$BASE/api/v1/uploads/init" "$TMP/initb.json"
BUPID="$(get_num "$HTTP_BODY" upload_id)"
check "哈希不符 init 新建会话" test -n "$BUPID" && test "$BUPID" != "0"
check "上传全部（内容正确）分块成功" upload_range "$BIG" "$BUPID" "$CHUNK" 0 "$TOTAL"
http_json POST "$BASE/api/v1/uploads/$BUPID/complete"
check "哈希不符 complete 返回 422" test "$HTTP_CODE" = "422"
check "422 响应含非空 invalid" test -n "$(get_invalid "$HTTP_BODY")"

# ---------------------------------------------------------------------------
echo "== 7. 取消会话（DELETE → 204）=="
printf '{"name":"cancel.bin","size":%s,"chunk_size":%s,"hash":""}' "$SIZE" "$CHUNK" > "$TMP/initc.json"
http_json POST "$BASE/api/v1/uploads/init" "$TMP/initc.json"
CUPID="$(get_num "$HTTP_BODY" upload_id)"
http_json DELETE "$BASE/api/v1/uploads/$CUPID"
check "DELETE 会话返回 204" test "$HTTP_CODE" = "204"
http_json GET "$BASE/api/v1/uploads/$CUPID"
check "DELETE 后再 GET 返回 404" test "$HTTP_CODE" = "404"

echo
echo "通过 $PASS 项，失败 $FAIL 项"
if [ "$FAIL" -eq 0 ]; then
  echo "冒烟测试全部通过"
  exit 0
fi
exit 1
