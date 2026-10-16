#!/usr/bin/env bash
# 云匣 CloudVault —— 目录树 + 按路径下载 冒烟脚本
# 覆盖：目录创建(201/幂等200)、穿越/非法路径拒绝(403/400)、
#       按路径下载(200/404/403)、镜像树落位
# 依赖：curl、sha256sum、mktemp；BASE 环境变量可指定服务端地址。
#
# 用法：先启动服务端，再 BASE=http://127.0.0.1:8080 bash testdata/smoke_dirs.sh

set -u
BASE="${BASE:-http://127.0.0.1:8080}"
pass=0
fail=0

code_of() { curl -s -o /dev/null -w '%{http_code}' "$@"; }
fetch_bin() { curl -s -o "$1" -w '%{http_code}' "${@:2}"; }

check() { # check <名称> <期望码> <实际码>
  if [ "$2" = "$3" ]; then
    echo "  ok   $1 (HTTP $3)"
    pass=$((pass+1))
  else
    echo "  FAIL $1 期望 $2 实际 $3"
    fail=$((fail+1))
  fi
}

echo "== smoke_dirs @ $BASE =="

# step1: 创建目录
code=$(curl -s -X POST "$BASE/api/v1/dirs" -H 'Content-Type: application/json' \
  -d '{"path":"docs/backup"}')
check "创建目录(201)" "201" "$code"

# step2: 重复创建 = 幂等 200
code=$(curl -s -X POST "$BASE/api/v1/dirs" -H 'Content-Type: application/json' \
  -d '{"path":"docs/backup"}')
check "重复创建目录(200)" "200" "$code"

# step3: 路径穿越 -> 403
code=$(code_of -X POST "$BASE/api/v1/dirs" -H 'Content-Type: application/json' \
  -d '{"path":"../etc"}')
check "穿越拒绝(403)" "403" "$code"

# step4: 绝对路径 -> 403
code=$(code_of -X POST "$BASE/api/v1/dirs" -H 'Content-Type: application/json' \
  -d '{"path":"/etc/x"}')
check "绝对路径拒绝(403)" "403" "$code"

# step5: 非法字符 -> 400
code=$(code_of -X POST "$BASE/api/v1/dirs" -H 'Content-Type: application/json' \
  -d '{"path":"a//b"}')
check "空路径段拒绝(400)" "400" "$code"

# step6: 整文件上传进目录（X-CV-Dir）
tmp=$(mktemp)
printf 'hello cloudvault dirs\n' > "$tmp"
code=$(curl -s -X POST "$BASE/api/v1/files" \
  -H "X-CV-Name: note.txt" -H "X-CV-Dir: docs%2Fbackup" \
  --data-binary @"$tmp")
check "上传进目录(201)" "201" "$code"

# step7: 按路径下载并比对内容
dl=$(mktemp)
code=$(fetch_bin "$dl" "$BASE/api/v1/download?path=docs%2Fbackup%2Fnote.txt")
check "按路径下载(200)" "200" "$code"
if [ "$(sha256sum < "$tmp" | cut -d' ' -f1)" = "$(sha256sum < "$dl" | cut -d' ' -f1)" ]; then
  echo "  ok   下载内容与上传一致"
  pass=$((pass+1))
else
  echo "  FAIL 下载内容不一致"
  fail=$((fail+1))
fi

# step8: 未记录文件 -> 404
code=$(code_of "$BASE/api/v1/download?path=docs%2Fbackup%2Fghost.txt")
check "未记录文件(404)" "404" "$code"

# step9: 下载路径穿越 -> 403
code=$(code_of "$BASE/api/v1/download?path=..%2Fsecret")
check "下载穿越拒绝(403)" "403" "$code"

# step10: 下载非法路径 -> 400
code=$(code_of "$BASE/api/v1/download?path=a%2F%2Fb")
check "下载非法路径(400)" "400" "$code"

rm -f "$tmp" "$dl"
echo "== 结果：pass=$pass fail=$fail =="
[ "$fail" -eq 0 ]
