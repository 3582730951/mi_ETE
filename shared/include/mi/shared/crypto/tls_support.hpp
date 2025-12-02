#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mi::shared::crypto
{
struct CertChainResult
{
    bool ok = false;
    bool selfSigned = false;
    std::string subject;
    std::string issuer;
    std::string fingerprintHex;
    std::wstring error;
};

// 计算输入数据的 SHA-256（十六进制与原始字节）
std::vector<std::uint8_t> Sha256(const std::vector<std::uint8_t>& data);
std::string Sha256Hex(const std::vector<std::uint8_t>& data);

// 加载并校验证书链（PFX/PKCS12），允许自签时自动豁免未信任根错误
CertChainResult ValidatePfxChain(const std::vector<std::uint8_t>& pfxBytes,
                                 const std::wstring& password,
                                 bool allowSelfSigned);

// 使用证书公钥（PFX/带公钥的证书）进行 RSA-OAEP 加密，密文不落地
bool EncryptWithCertificate(const std::vector<std::uint8_t>& pfxBytes,
                            const std::wstring& password,
                            const std::vector<std::uint8_t>& plain,
                            std::vector<std::uint8_t>& cipher);

// 使用 PFX 私钥执行 RSA-OAEP 解密，支持 CNG/Legacy Provider
bool DecryptWithPrivateKey(const std::vector<std::uint8_t>& pfxBytes,
                           const std::wstring& password,
                           const std::vector<std::uint8_t>& cipher,
                           std::vector<std::uint8_t>& plain);
}  // namespace mi::shared::crypto
