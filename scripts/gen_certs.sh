#!/usr/bin/env bash
set -euo pipefail

# 生成自签证书（根CA + 服务器 + 客户端），输出到 certs/ 目录
# 仅用于开发/测试，请勿用于生产

ROOT=certs
mkdir -p "$ROOT"
cd "$ROOT"

# CA
openssl genrsa -out ca.key 2048
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -subj "/CN=mi_ca" -out ca.crt

# server
openssl genrsa -out server.key 2048
openssl req -new -key server.key -subj "/CN=mi_server" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 825 -sha256

# client
openssl genrsa -out client.key 2048
openssl req -new -key client.key -subj "/CN=mi_client" -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt -days 825 -sha256

# 导出 pfx 便于签名/客户端导入
openssl pkcs12 -export -out client.pfx -inkey client.key -in client.crt -passout pass:changeit
openssl pkcs12 -export -out server.pfx -inkey server.key -in server.crt -passout pass:changeit

rm -f *.csr ca.srl
