@echo off
rem 生成约 6 MiB 的测试文本，用于验证跨分块上传/下载。
rem 服务端默认分块 5MB（5242880 字节），本文件会切成 2 块。
rem
rem 字节数算法：每行 = 36(前缀 "CloudVault chunk-boundary test line ") + 行号位数
rem   + 63(后缀 ": abcdefghijklmnopqrstuvwxyz 0123456789 padding padding padding")
rem   + 2(CRLF) = 101 + 行号位数；
rem   合计 = N×101 + Σ(1..N 的十进制位数)。
rem 本脚本取 N=59552：59552×101 + 286654 = 6301406 字节（约 6.01 MiB）。
rem 说明：cmd 的 echo 无法精确裁剪到指定字节，与 gen_big.sh 的 6291456 字节略有出入
rem   （+9950 字节，+0.16%），同样大于 5MB 分块阈值，不影响跨分块验证。
rem
rem 用法：
rem   gen_big.bat                 在当前目录生成 big.txt
rem   gen_big.bat D:\tmp\big.txt  指定输出路径
rem
rem 注意：big.txt 体积较大，已加入 .gitignore，不要把生成结果提交到仓库。
rem 内容刻意使用纯 ASCII：cmd 的代码页会把中文 echo 输出写成乱码，
rem 不影响分块验证（只看大小），中文场景请用 testdata 里的其他文件。

setlocal enabledelayedexpansion
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0big.txt"

rem 整段只做一次重定向（把 for 循环包在括号里），否则逐行追加会非常慢
> "%OUT%" (
    for /L %%i in (1,1,59552) do echo CloudVault chunk-boundary test line %%i: abcdefghijklmnopqrstuvwxyz 0123456789 padding padding padding
)

echo 已生成 %OUT%
for %%F in ("%OUT%") do echo 大小: %%~zF 字节 ^(目标 6301406 字节，约 6.01 MiB^)
endlocal
