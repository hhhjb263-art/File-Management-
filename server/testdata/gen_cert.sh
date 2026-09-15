#!/usr/bin/env bash
# 生成 HTTPS 自签名证书（服务端 TLS 用）
# 用法: ./gen_cert.sh [CN] [服务器IP]
#   默认: ./gen_cert.sh cloudvault 172.20.32.231
set -e
CN="${1:-cloudvault}"
IP="${2:-172.20.32.231}"
OUT="$(cd "$(dirname "$0")" && pwd)"

openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout "$OUT/tls_key.pem" -out "$OUT/tls_cert.pem" \
  -subj "/CN=${CN}" \
  -addext "subjectAltName=IP:${IP},DNS:localhost,IP:127.0.0.1" 2>/dev/null

chmod 600 "$OUT/tls_key.pem"
echo "已生成: $OUT/tls_cert.pem / $OUT/tls_key.pem"
echo "启动示例:"
echo "  ./build/cloudvault-server --data-dir=/home/user/cvdata \\"
echo "    --port=8080 --tls-port=8443 \\"
echo "    --tls-cert=$OUT/tls_cert.pem --tls-key=$OUT/tls_key.pem"
echo "客户端地址栏填: https://${IP}:8443 （并保持【信任自签名证书】勾选）"
