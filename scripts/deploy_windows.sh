#!/usr/bin/env bash
# 把正式客户端所需 Qt 运行时部署到 bin/，使 CloudVault.exe 可脱离 Qt 环境独立运行
# （可直接双击 / 拷到其他机器运行；bin/ 已在 .gitignore 中，克隆后需重新跑本脚本）
#
# 用法: bash scripts/deploy_windows.sh [Qt前缀] [MinGW前缀]
#   默认: Qt=D:/Qt/6.9.3/mingw_64  MinGW=D:/Qt/Tools/mingw1310_64
set -euo pipefail
QT="${1:-D:/Qt/6.9.3/mingw_64}"
MINGW="${2:-D:/Qt/Tools/mingw1310_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
EXE="$BIN/CloudVault.exe"

[ -f "$EXE" ] || { echo "错误：未找到 $EXE" >&2; echo "请先构建：cd build_official && qmake ../File.pro && mingw32-make -j4" >&2; exit 2; }
[ -x "$QT/bin/windeployqt.exe" ] || { echo "错误：未找到 $QT/bin/windeployqt.exe" >&2; exit 2; }

echo "==> 1/4 windeployqt（--qmldir 扫描 QML 导入，FluentWinUI3 风格必需）"
"$QT/bin/windeployqt.exe" --release --qmldir "$ROOT/qml" --no-translations "$EXE"

echo "==> 2/4 补齐 TLS 后端插件（自签名 HTTPS 必需；windeployqt 有时只拷 2 个）"
mkdir -p "$BIN/tls"
for f in "$QT"/plugins/tls/*.dll; do [ -f "$f" ] && cp -Lf "$f" "$BIN/tls/"; done
ls "$BIN/tls" | sed 's/^/    /'

echo "==> 3/4 补齐 MinGW 运行库（libgcc/libstdc++/libwinpthread，跨机器运行必需）"
for d in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
  for cand in "$MINGW/bin/$d" "$QT/bin/$d"; do
    if [ -f "$cand" ]; then cp -Lf "$cand" "$BIN/$d"; echo "    ✓ $d"; break; fi
  done
done

echo "==> 4/4 校验 exe 直接依赖是否都落在 bin/ 下"
miss=0
for d in $(objdump -p "$EXE" | awk '/DLL Name:/{print $3}' | sort -u); do
  case "$d" in KERNEL32.dll|SHELL32.dll|msvcrt.dll|USER32.dll|ADVAPI32.dll|WS2_32.dll|GDI32.dll|ole32.dll) continue;; esac
  [ -f "$BIN/$d" ] || { echo "    ✗ 缺少 $d"; miss=1; }
done
[ "$miss" -eq 0 ] && echo "    ✓ 全部直接依赖已就位"

echo ""
echo "完成：$EXE（$(du -sh "$BIN" | awk '{print $1}')，含 $(ls "$BIN"/*.dll | wc -l) 个 DLL + QML 模块）"
echo "现在可直接双击运行，或整目录拷到其他 Windows 机器（无需安装 Qt）。"
