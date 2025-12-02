#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mi::shared::crypto
{
struct WhiteboxKeyInfo
{
    std::vector<std::uint8_t> keyParts;
};

std::vector<std::uint8_t> Encrypt(const std::vector<std::uint8_t>& plain, const WhiteboxKeyInfo& keyInfo);
std::vector<std::uint8_t> Decrypt(const std::vector<std::uint8_t>& cipher, const WhiteboxKeyInfo& keyInfo);

// 从环境变量分片加载密钥，例如 MI_AES_KEY_PART0/1/2...
WhiteboxKeyInfo BuildKeyFromEnv(const std::string& prefix = "MI_AES_KEY_PART");

// 与动态分量混合生成新密钥（不会修改原始 keyInfo）
WhiteboxKeyInfo MixKey(const WhiteboxKeyInfo& base, const std::vector<std::uint8_t>& dynamic);
}  // namespace mi::shared::crypto
