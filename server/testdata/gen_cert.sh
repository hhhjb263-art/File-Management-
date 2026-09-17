#!/usr/bin/env bash
# 生成 HTTPS 自签名证书（服务端 TLS 用）
# 用法: ./gen_cert.sh [CN] [服务器IP或域名]
#   默认: ./gen_cert.sh cloudvault 172.20.32.231
# 说明: IP/域名会自动清洗（去首尾空白、去结尾 '/'、去 'IP:' 前缀），生成后回显 SAN 与指纹便于核对；
#       openssl 的报错不再被吞掉——失败会明确报错并以非零码退出，且不会留下半成品或沿用旧证书而不自知。
set -euo pipefail
CN="${1:-cloudvault}"
RAW="${2:-172.20.32.231}"
OUT="$(cd "$(dirname "$0")" && pwd)"

HOST="$(printf '%s' "$RAW" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's#/*$##' -e 's/^IP://')"
if [ -z "$HOST" ]; then
  echo "错误：服务器 IP/域名不能为空（收到 '$RAW'）" >&2
  exit 2
fi
if [ "$HOST" != "$RAW" ]; then
  echo "提示：已把参数 '$RAW' 清洗为 '$HOST'"
fi

if printf '%s' "$HOST" | grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){3}$'; then
  SAN="IP:${HOST},IP:127.0.0.1,DNS:localhost"
else
  SAN="DNS:${HOST},DNS:localhost,IP:127.0.0.1"
fi

if ! command -v openssl >/dev/null 2>&1; then
  echo "错误：找不到 openssl，请先安装（Debian/Ubuntu: sudo apt install openssl）" >&2
  exit 2
fi

TMPC="$OUT/.tls_cert.pem.tmp"
TMPK="$OUT/.tls_key.pem.tmp"
rm -f "$TMPC" "$TMPK"

# 不重定向 stderr：让 openssl 的真实报错可见（例如 'bad ip address: value=...'）
if ! openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
     -keyout "$TMPK" -out "$TMPC" \
     -subj "/CN=${CN}" \
     -addext "subjectAltName=${SAN}"; then
  echo "" >&2
  echo "错误：证书生成失败（原因见上面的 openssl 输出）。常见原因：" >&2
  echo "  · IP/域名写错，例如多写了结尾斜杠（bad ip address）" >&2
  echo "  · 目标目录不可写" >&2
  rm -f "$TMPC" "$TMPK"
  exit 1
fi

mv -f "$TMPC" "$OUT/tls_cert.pem"
mv -f "$TMPK" "$OUT/tls_key.pem"
chmod 600 "$OUT/tls_key.pem"

echo ""
echo "已生成: $OUT/tls_cert.pem"
echo "        $OUT/tls_key.pem（权限 600）"
echo "证书 SAN（客户端连接的地址必须在此列表内）:"
openssl x509 -in "$OUT/tls_cert.pem" -noout -text 2>/dev/null \
  | sed -n '/Subject Alternative Name/{n;p;}' | sed 's/^[[:space:]]*/  /'
echo "证书指纹(SHA-256)（可与客户端【证书…】显示的指纹逐段核对）:"
openssl x509 -in "$OUT/tls_cert.pem" -noout -fingerprint -sha256 2>/dev/null | sed 's/^/  /'
echo ""
echo "启动示例:"
echo "  ./build/cloudvault-server --data-dir=/home/user/cvdata \\"
echo "    --port=8080 --tls-port=8443 \\"
echo "    --tls-cert=$OUT/tls_cert.pem --tls-key=$OUT/tls_key.pem \\"
echo "    --auth-token=你的口令"
echo "客户端地址栏填: https://${HOST}:8443 （令牌框填同一口令；首次连接在弹出的确认框里核对指纹后点【信任】）"
