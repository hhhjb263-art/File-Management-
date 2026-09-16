#!/usr/bin/env bash
# CloudVault 鉴权绕过清单 + constantTimeEqual 长度边界 验收测试。
#
# 前置条件
# --------
# 1) 主用例：服务端已在 $CV_BASE 运行，且【必须】以 --auth-token=$CV_TOKEN 启用鉴权，例如：
#      ./build/cloudvault-server --data-dir /tmp/cv-auth --port 8080 --auth-token SECRET123
# 2) 长度边界用例：本脚本会【自动另起一个独立实例】（token = A×300，默认端口 8081），
#    需要已构建的服务端可执行文件（CV_SERVER_BIN 或 CV_SERVER，默认 ./build/cloudvault-server）。
#    可用 CV_SKIP_LEN_BOUNDARY=1 跳过该用例。
# 本脚本只能在 Linux/Debian 上对真实服务端运行（Windows 无法运行服务端）。
#
# 用法
# ----
#   bash auth_bypass_test.sh
#   CV_TOKEN=mysecret CV_BASE=http://127.0.0.1:8443 bash auth_bypass_test.sh
#   CV_SERVER_BIN=./build/cloudvault-server CV_TEST_LEN_BOUNDARY_PORT=8091 bash auth_bypass_test.sh
#
# 退出码：全部通过 0；存在 FAIL 1（SKIP 不计入失败）。
set -uo pipefail

T=${CV_TOKEN:-SECRET123}
B=${CV_BASE:-http://127.0.0.1:8080}
SRV=${CV_SERVER_BIN:-${CV_SERVER:-./build/cloudvault-server}}
LB_PORT=${CV_TEST_LEN_BOUNDARY_PORT:-8081}
LB_DIR=${CV_TEST_LEN_BOUNDARY_DIR:-/tmp/cv-len-boundary}

pass=0
fail=0
skip=0

chk() {
  local desc=$1 exp=$2
  shift 2
  local code
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" || echo "000")
  if [ "$code" = "$exp" ]; then
    printf 'PASS  %-40s -> %s\n' "$desc" "$code"
    pass=$((pass + 1))
  else
    printf 'FAIL  %-40s -> 期望 %s 实得 %s\n' "$desc" "$exp" "$code"
    fail=$((fail + 1))
  fi
}

# 生成 n 个 'A'
rep_a() {
  local n=$1 out=""
  for ((i = 0; i < n; i++)); do out+="A"; done
  printf '%s' "$out"
}

echo "== 主用例  target=$B  token=$T =="

echo "-- 期望 401：鉴权必须拒绝 --"
chk "无 Authorization 头"        401 "$B/api/v1/files"
chk "Authorization 空值"         401 -H 'Authorization:' "$B/api/v1/files"
chk "只有 Bearer 无 token"       401 -H 'Authorization: Bearer' "$B/api/v1/files"
chk "Bearer 后仅空格"            401 -H 'Authorization: Bearer ' "$B/api/v1/files"
chk "小写 bearer"                401 -H "Authorization: bearer $T" "$B/api/v1/files"
chk "混合大小写 BeArEr"          401 -H "Authorization: BeArEr $T" "$B/api/v1/files"
chk "错误 token"                 401 -H "Authorization: Bearer nope" "$B/api/v1/files"
chk "token 带引号"               401 -H "Authorization: Bearer \"$T\"" "$B/api/v1/files"
chk "token 尾部空格"             401 -H "Authorization: Bearer $T " "$B/api/v1/files"
chk "大写路径 /API/v1/files"     401 "$B/API/v1/files"
chk "大写 /HEALTHZ"              401 "$B/HEALTHZ"
chk "HEAD 打受保护路径"          401 -I "$B/api/v1/files"
chk "OPTIONS 打受保护路径"       401 -X OPTIONS "$B/api/v1/files"
chk "/healthz 尾斜杠"            401 "$B/healthz/"
chk "/healthz 双斜杠"            401 "$B//healthz"

echo "-- 期望 200：合法访问 / 探活豁免 --"
chk "正确 Bearer"                200 -H "Authorization: Bearer $T" "$B/api/v1/files"
chk "正确 X-CV-Token"            200 -H "X-CV-Token: $T" "$B/api/v1/files"
chk "/healthz 豁免"              200 "$B/healthz"
chk "/healthz?x=1 豁免"          200 "$B/healthz?x=1"

# ---------------------------------------------------------------------------
# constantTimeEqual 长度边界回归（最高价值）
#   修复前 bug：diff 以 unsigned char 折入长度差 → 长度差为 256 的整数倍时被截断为 0；
#   若提交的是 secret 的真前缀（此处 A×44 vs A×300，Δlen=256），会被误判为相等（200）。
#   修复后必须 401。
# ---------------------------------------------------------------------------
echo "-- constantTimeEqual 长度边界回归（独立实例，token=A×300）--"
if [ "${CV_SKIP_LEN_BOUNDARY:-0}" = "1" ]; then
  echo "SKIP  已由 CV_SKIP_LEN_BOUNDARY=1 跳过"
  skip=$((skip + 1))
elif [ ! -x "$SRV" ]; then
  echo "SKIP  找不到可执行服务端 '$SRV'（用 CV_SERVER_BIN=... 指定）"
  skip=$((skip + 1))
else
  SECRET=$(rep_a 300)
  rm -rf "$LB_DIR"
  mkdir -p "$LB_DIR"
  "$SRV" --data-dir "$LB_DIR" --port "$LB_PORT" --auth-token "$SECRET" >"$LB_DIR/server.log" 2>&1 &
  LB_PID=$!
  LB="http://127.0.0.1:$LB_PORT"
  up=0
  for _ in $(seq 1 50); do
    if curl -s -o /dev/null --max-time 1 "$LB/healthz"; then up=1; break; fi
    sleep 0.1
  done
  if [ "$up" != "1" ]; then
    echo "SKIP  长度边界实例未就绪（见 $LB_DIR/server.log）"
    skip=$((skip + 1))
    kill "$LB_PID" 2>/dev/null
  else
    chk "长度边界: 真前缀 A×44 (Δlen=256) 应 401" 401 -H "Authorization: Bearer $(rep_a 44)" "$LB/api/v1/files"
    chk "长度边界: 正确 token A×300 应 200"       200 -H "Authorization: Bearer $SECRET" "$LB/api/v1/files"
    chk "长度边界: A×45 应 401"                   401 -H "Authorization: Bearer $(rep_a 45)" "$LB/api/v1/files"
    chk "长度边界: A×299 应 401"                  401 -H "Authorization: Bearer $(rep_a 299)" "$LB/api/v1/files"
    kill "$LB_PID" 2>/dev/null
  fi
fi

echo "-----------------------------------------"
printf 'TOTAL: %d PASS, %d FAIL, %d SKIP\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ] || exit 1
