param(
    [string]$OutputDir = "certs",
    [string]$Password = "changeit"
)

$ErrorActionPreference = "Stop"

function Ensure-OpenSsl {
    $openssl = Get-Command "openssl" -ErrorAction SilentlyContinue
    if (-not $openssl) {
        Write-Warning "未找到 openssl，无法生成自签证书。请安装 OpenSSL 或在有权限的环境执行。"
        exit 1
    }
    return $openssl
}

Ensure-OpenSsl | Out-Null

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Push-Location $OutputDir

& openssl genrsa -out ca.key 2048
& openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 -subj "/CN=mi_ca" -out ca.crt

& openssl genrsa -out server.key 2048
& openssl req -new -key server.key -subj "/CN=mi_server" -out server.csr
& openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 825 -sha256

& openssl genrsa -out client.key 2048
& openssl req -new -key client.key -subj "/CN=mi_client" -out client.csr
& openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out client.crt -days 825 -sha256

& openssl pkcs12 -export -out client.pfx -inkey client.key -in client.crt -passout pass:$Password
& openssl pkcs12 -export -out server.pfx -inkey server.key -in server.crt -passout pass:$Password

Remove-Item -Force -ErrorAction SilentlyContinue *.csr, ca.srl | Out-Null
Pop-Location

Write-Host "[certs] 自签证书已生成到 $OutputDir（密码: $Password）"
