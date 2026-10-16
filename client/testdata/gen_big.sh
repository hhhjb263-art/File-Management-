#!/usr/bin/env bash
# 生成约 6MB（6291456 字节 = 6 MiB）的测试文本，用于验证跨分块上传/下载。
# 服务端默认分块 5MB（5242880 字节），6MiB 会被切成 2 块。
#
# 用法：
#   ./gen_big.sh                 # 在脚本所在目录生成 big.txt
#   ./gen_big.sh /tmp/big.txt    # 指定输出路径
#
# 注意：big.txt 体积较大，已加入 .gitignore，不要把生成结果提交到仓库。

set -euo pipefail

TARGET=6291456
OUT="${1:-$(cd "$(dirname "$0")" && pwd)/big.txt}"
TMP="$(mktemp)"

cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

# 先造一批带中文的重复行，再拼接并精确裁剪到目标大小
for i in $(seq 1 52000); do
  printf '云匣 CloudVault 跨分块测试行 %06d：abcdefghijklmnopqrstuvwxyz 0123456789 中文填充内容用于凑够块大小\n' "$i"
done > "$TMP"

for i in 1 2 3 4 5 6; do
  cat "$TMP"
done | head -c "$TARGET" > "$OUT"

echo "已生成 $OUT"
ls -lh "$OUT"
