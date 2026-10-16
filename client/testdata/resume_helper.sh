#!/usr/bin/env bash
# 云匣 CloudVault 续传验证小工具（仅用于模拟"下载被中断"，无需真断网）
#
# 把源文件的前 N 字节写成 <输出路径>.part；之后在客户端点【分块下载（断点续传）】
# 并选择 <输出路径>，客户端会检测到 .part 并从断点（offset=N）继续拉取剩余字节，
# 最终把 .part 重命名为正式文件。如此即可验证分块下载的断点续传。
#
# 上传续传的验证更简单：选一个大文件点【分块上传（断点续传）】，传输中途点【取消上传】，
# 再选同一文件重传——只要 size+mtime+hash 不变，就会跳过已传分块、从断点继续。
#
# 用法：
#   ./resume_helper.sh <源文件> <保留字节数> <输出路径>
# 示例（保留前 1 MiB）：
#   ./resume_helper.sh big.txt 1048576 big_downloaded.txt

usage() { echo "用法: $0 <源文件> <保留字节数> <输出路径>" >&2; exit 1; }
[ "$#" -eq 3 ] || usage

src="$1"; keep="$2"; out="$3"
if [ ! -f "$src" ]; then echo "源文件不存在: $src" >&2; exit 1; fi

head -c "$keep" "$src" > "$out.part"
echo "已生成 $out.part（前 $keep 字节）。在客户端点【分块下载（断点续传）】选「$out」即可从断点续传。"
