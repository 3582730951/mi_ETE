#include "mi/shared/crypto/cert_store.hpp"

#include <cstdlib>
#include <string>
#include <cctype>

namespace
{
std::vector<std::uint8_t> DecodeBase64(const std::string& input)
{
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : input)
    {
        if (std::isspace(c))
        {
            continue;
        }
        if (c == '=')
        {
            break;
        }
        const auto pos = alphabet.find(c);
        if (pos == std::string::npos)
        {
            continue;
        }
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<std::uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
}  // namespace

namespace mi::shared::crypto
{
std::vector<std::uint8_t> LoadCertFromEnv(const std::string& envVar)
{
    const char* env = std::getenv(envVar.c_str());
    if (env == nullptr)
    {
        return {};
    }
    return DecodeBase64(env);
}

std::vector<std::uint8_t> LoadCertFromBase64(const std::string& base64)
{
    return DecodeBase64(base64);
}

std::vector<std::uint8_t> LoadCertFromConfig(const std::wstring& base64)
{
    return DecodeBase64(std::string(base64.begin(), base64.end()));
}
}  // namespace mi::shared::crypto
