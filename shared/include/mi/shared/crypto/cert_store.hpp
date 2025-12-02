#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace mi::shared::crypto
{
// 证书仅以内存字节数组存在，不落地
std::vector<std::uint8_t> LoadCertFromEnv(const std::string& envVar = "MI_CERT_B64");
std::vector<std::uint8_t> LoadCertFromBase64(const std::string& base64);
std::vector<std::uint8_t> LoadCertFromConfig(const std::wstring& base64);
}  // namespace mi::shared::crypto
