#include "mi/shared/crypto/whitebox_aes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Block = std::array<std::uint8_t, 16>;

constexpr std::array<std::uint8_t, 256> kSBox = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16};

constexpr std::array<std::uint8_t, 10> kRcon = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};

std::array<std::uint8_t, 256> BuildMul2()
{
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i)
    {
        const std::uint8_t v = static_cast<std::uint8_t>(i);
        table[i] = static_cast<std::uint8_t>((v << 1) ^ (((v >> 7) & 1) * 0x1B));
    }
    return table;
}

std::array<std::uint8_t, 256> BuildMul3(const std::array<std::uint8_t, 256>& mul2)
{
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i)
    {
        table[i] = static_cast<std::uint8_t>(mul2[i] ^ static_cast<std::uint8_t>(i));
    }
    return table;
}

std::uint32_t Pack(const std::uint8_t a, const std::uint8_t b, const std::uint8_t c, const std::uint8_t d)
{
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
}

std::uint32_t RotWord(std::uint32_t w)
{
    return (w << 8) | (w >> 24);
}

std::uint32_t SubWord(std::uint32_t w)
{
    return (static_cast<std::uint32_t>(kSBox[(w >> 24) & 0xFF]) << 24) |
           (static_cast<std::uint32_t>(kSBox[(w >> 16) & 0xFF]) << 16) |
           (static_cast<std::uint32_t>(kSBox[(w >> 8) & 0xFF]) << 8) |
           static_cast<std::uint32_t>(kSBox[w & 0xFF]);
}

std::array<std::array<std::uint32_t, 4>, 11> ExpandKey(const Block& key)
{
    std::array<std::uint32_t, 44> w{};
    for (int i = 0; i < 4; ++i)
    {
        w[static_cast<std::size_t>(i)] = Pack(key[static_cast<std::size_t>(4 * i)],
                                              key[static_cast<std::size_t>(4 * i + 1)],
                                              key[static_cast<std::size_t>(4 * i + 2)],
                                              key[static_cast<std::size_t>(4 * i + 3)]);
    }

    for (int i = 4; i < 44; ++i)
    {
        std::uint32_t temp = w[static_cast<std::size_t>(i - 1)];
        if (i % 4 == 0)
        {
            temp = SubWord(RotWord(temp)) ^ (static_cast<std::uint32_t>(kRcon[(i / 4) - 1]) << 24);
        }
        w[static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i - 4)] ^ temp;
    }

    std::array<std::array<std::uint32_t, 4>, 11> roundKeys{};
    for (int r = 0; r < 11; ++r)
    {
        for (int i = 0; i < 4; ++i)
        {
            roundKeys[static_cast<std::size_t>(r)][static_cast<std::size_t>(i)] =
                w[static_cast<std::size_t>(r * 4 + i)];
        }
    }
    return roundKeys;
}

std::array<std::uint8_t, 16> RoundKeyBytes(const std::array<std::uint32_t, 4>& rk)
{
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i)
    {
        const std::uint32_t w = rk[static_cast<std::size_t>(i)];
        out[static_cast<std::size_t>(4 * i + 0)] = static_cast<std::uint8_t>((w >> 24) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 1)] = static_cast<std::uint8_t>((w >> 16) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 2)] = static_cast<std::uint8_t>((w >> 8) & 0xFF);
        out[static_cast<std::size_t>(4 * i + 3)] = static_cast<std::uint8_t>(w & 0xFF);
    }
    return out;
}

std::uint64_t HashKey(const std::vector<std::uint8_t>& keyParts, std::uint64_t salt)
{
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ salt;
    for (std::size_t i = 0; i < keyParts.size(); ++i)
    {
        h ^= static_cast<std::uint64_t>(keyParts[i]) << ((i % 8) * 8);
        h = (h << 13) | (h >> 51);
        h = h * 0xC2B2AE3D27D4EB4FULL + 0x165667B19E3779F9ULL;
    }
    return h;
}

struct RoundTables
{
    std::array<std::array<std::uint32_t, 256>, 4> tables{};
    std::array<std::uint32_t, 4> maskedRoundKey{};
};

class WhiteboxTables
{
public:
    WhiteboxTables(const Block& key, std::uint64_t maskSeed, std::uint64_t encodingSeed)
        : roundKeys_(ExpandKey(key)), finalKeyBytes_(RoundKeyBytes(roundKeys_[10]))
    {
        BuildExternalEncoding(encodingSeed);
        BuildTables(maskSeed);
    }

    Block EncryptBlock(const Block& input) const
    {
        Block encoded{};
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            encoded[i] = inputEncoding_[input[i]];
        }

        std::uint32_t s0 = Pack(encoded[0], encoded[1], encoded[2], encoded[3]) ^ roundKeys_[0][0];
        std::uint32_t s1 = Pack(encoded[4], encoded[5], encoded[6], encoded[7]) ^ roundKeys_[0][1];
        std::uint32_t s2 = Pack(encoded[8], encoded[9], encoded[10], encoded[11]) ^ roundKeys_[0][2];
        std::uint32_t s3 = Pack(encoded[12], encoded[13], encoded[14], encoded[15]) ^ roundKeys_[0][3];

        for (std::size_t r = 0; r < rounds_.size(); ++r)
        {
            const RoundTables& rt = rounds_[r];
            std::uint32_t t0 = rt.tables[0][(s0 >> 24) & 0xFF] ^ rt.tables[1][(s1 >> 16) & 0xFF] ^
                               rt.tables[2][(s2 >> 8) & 0xFF] ^ rt.tables[3][s3 & 0xFF] ^
                               rt.maskedRoundKey[0];
            std::uint32_t t1 = rt.tables[0][(s1 >> 24) & 0xFF] ^ rt.tables[1][(s2 >> 16) & 0xFF] ^
                               rt.tables[2][(s3 >> 8) & 0xFF] ^ rt.tables[3][s0 & 0xFF] ^
                               rt.maskedRoundKey[1];
            std::uint32_t t2 = rt.tables[0][(s2 >> 24) & 0xFF] ^ rt.tables[1][(s3 >> 16) & 0xFF] ^
                               rt.tables[2][(s0 >> 8) & 0xFF] ^ rt.tables[3][s1 & 0xFF] ^
                               rt.maskedRoundKey[2];
            std::uint32_t t3 = rt.tables[0][(s3 >> 24) & 0xFF] ^ rt.tables[1][(s0 >> 16) & 0xFF] ^
                               rt.tables[2][(s1 >> 8) & 0xFF] ^ rt.tables[3][s2 & 0xFF] ^
                               rt.maskedRoundKey[3];
            s0 = t0;
            s1 = t1;
            s2 = t2;
            s3 = t3;
        }

        std::array<std::uint8_t, 16> state{};
        state[0] = static_cast<std::uint8_t>((s0 >> 24) & 0xFF);
        state[1] = static_cast<std::uint8_t>((s0 >> 16) & 0xFF);
        state[2] = static_cast<std::uint8_t>((s0 >> 8) & 0xFF);
        state[3] = static_cast<std::uint8_t>(s0 & 0xFF);
        state[4] = static_cast<std::uint8_t>((s1 >> 24) & 0xFF);
        state[5] = static_cast<std::uint8_t>((s1 >> 16) & 0xFF);
        state[6] = static_cast<std::uint8_t>((s1 >> 8) & 0xFF);
        state[7] = static_cast<std::uint8_t>(s1 & 0xFF);
        state[8] = static_cast<std::uint8_t>((s2 >> 24) & 0xFF);
        state[9] = static_cast<std::uint8_t>((s2 >> 16) & 0xFF);
        state[10] = static_cast<std::uint8_t>((s2 >> 8) & 0xFF);
        state[11] = static_cast<std::uint8_t>(s2 & 0xFF);
        state[12] = static_cast<std::uint8_t>((s3 >> 24) & 0xFF);
        state[13] = static_cast<std::uint8_t>((s3 >> 16) & 0xFF);
        state[14] = static_cast<std::uint8_t>((s3 >> 8) & 0xFF);
        state[15] = static_cast<std::uint8_t>(s3 & 0xFF);

        for (std::uint8_t& b : state)
        {
            b = kSBox[b];
        }

        std::array<std::uint8_t, 16> tmp{};
        tmp[0] = state[0];
        tmp[1] = state[5];
        tmp[2] = state[10];
        tmp[3] = state[15];

        tmp[4] = state[4];
        tmp[5] = state[9];
        tmp[6] = state[14];
        tmp[7] = state[3];

        tmp[8] = state[8];
        tmp[9] = state[13];
        tmp[10] = state[2];
        tmp[11] = state[7];

        tmp[12] = state[12];
        tmp[13] = state[1];
        tmp[14] = state[6];
        tmp[15] = state[11];

        for (std::size_t i = 0; i < tmp.size(); ++i)
        {
            tmp[i] = static_cast<std::uint8_t>(tmp[i] ^ finalKeyBytes_[i]);
            tmp[i] = outputEncoding_[tmp[i]];
        }
        return tmp;
    }

private:
    void BuildExternalEncoding(std::uint64_t seed)
    {
        std::vector<std::uint8_t> values(256);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            values[i] = static_cast<std::uint8_t>(i);
        }

        std::mt19937_64 gen(seed);
        std::shuffle(values.begin(), values.end(), gen);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            inputEncoding_[static_cast<std::size_t>(i)] = values[i];
            inputDecoding_[values[i]] = static_cast<std::uint8_t>(i);
        }

        std::shuffle(values.begin(), values.end(), gen);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            outputEncoding_[static_cast<std::size_t>(i)] = values[i];
            outputDecoding_[values[i]] = static_cast<std::uint8_t>(i);
        }
    }

    void BuildTables(std::uint64_t maskSeed)
    {
        const auto mul2 = BuildMul2();
        const auto mul3 = BuildMul3(mul2);

        for (int r = 1; r <= 9; ++r)
        {
            RoundTables rt{};
            // 派生掩码：简单双倍哈希，保证不同轮次表异构。
            std::uint32_t m0 = static_cast<std::uint32_t>((maskSeed >> (r % 8)) ^ (0xA5A5A5A5u + r * 97u));
            std::uint32_t m1 = static_cast<std::uint32_t>((maskSeed >> ((r + 1) % 8)) ^ (0x3C3C3C3Cu + r * 31u));
            std::uint32_t m2 = static_cast<std::uint32_t>((maskSeed >> ((r + 2) % 8)) ^ (0x5A5A5A5Au + r * 17u));
            std::uint32_t m3 = static_cast<std::uint32_t>((maskSeed >> ((r + 3) % 8)) ^ (0xC3C3C3C3u + r * 11u));
            const std::uint32_t combined = m0 ^ m1 ^ m2 ^ m3;

            for (int x = 0; x < 256; ++x)
            {
                const std::uint8_t s = kSBox[x];
                const std::uint32_t t0 = Pack(mul2[s], s, s, mul3[s]) ^ m0;
                const std::uint32_t t1 = Pack(mul3[s], mul2[s], s, s) ^ m1;
                const std::uint32_t t2 = Pack(s, mul3[s], mul2[s], s) ^ m2;
                const std::uint32_t t3 = Pack(s, s, mul3[s], mul2[s]) ^ m3;
                rt.tables[0][static_cast<std::size_t>(x)] = t0;
                rt.tables[1][static_cast<std::size_t>(x)] = t1;
                rt.tables[2][static_cast<std::size_t>(x)] = t2;
                rt.tables[3][static_cast<std::size_t>(x)] = t3;
            }

            const std::size_t idx = static_cast<std::size_t>(r);
            rt.maskedRoundKey[0] = roundKeys_[idx][0] ^ combined;
            rt.maskedRoundKey[1] = roundKeys_[idx][1] ^ combined;
            rt.maskedRoundKey[2] = roundKeys_[idx][2] ^ combined;
            rt.maskedRoundKey[3] = roundKeys_[idx][3] ^ combined;
            rounds_[static_cast<std::size_t>(r - 1)] = rt;
        }
    }

    std::array<std::array<std::uint32_t, 4>, 11> roundKeys_{};
    std::array<RoundTables, 9> rounds_{};
    std::array<std::uint8_t, 16> finalKeyBytes_{};
    std::array<std::uint8_t, 256> inputEncoding_{};
    std::array<std::uint8_t, 256> inputDecoding_{};
    std::array<std::uint8_t, 256> outputEncoding_{};
    std::array<std::uint8_t, 256> outputDecoding_{};
};

std::uint32_t RotateLeft32(std::uint32_t value, unsigned int bits)
{
    const unsigned int masked = bits & 31u;
    return (value << masked) | (value >> (32u - masked));
}

Block DeriveMaterial(const mi::shared::crypto::WhiteboxKeyInfo& keyInfo, std::uint32_t salt)
{
    std::uint32_t state = salt ^ 0xA5C35A7Bu;
    for (std::size_t i = 0; i < keyInfo.keyParts.size(); ++i)
    {
        state ^= static_cast<std::uint32_t>(keyInfo.keyParts[i]) << ((i % 4) * 8);
        state = RotateLeft32(state + 0x9E3779B9u + static_cast<std::uint32_t>(i * 11u), 5);
        state ^= (state >> 13) | (state << 19);
    }

    if (keyInfo.keyParts.empty())
    {
        state ^= 0xC6EF3720u;
    }

    Block material{};
    for (std::size_t i = 0; i < material.size(); ++i)
    {
        state = RotateLeft32(state ^ 0x7F4A7C15u ^ static_cast<std::uint32_t>(i * 23u), 3);
        state += 0x6D2B79F5u + static_cast<std::uint32_t>(i * 7u);
        material[i] = static_cast<std::uint8_t>((state >> ((i % 4) * 8)) & 0xFFu);
    }

    // 额外扰动：将 keyParts 反转后混入，模拟白盒拆分与混淆。
    for (std::size_t i = 0; i < keyInfo.keyParts.size(); ++i)
    {
        const std::uint8_t part = keyInfo.keyParts[keyInfo.keyParts.size() - 1 - i];
        material[i % material.size()] = static_cast<std::uint8_t>(material[i % material.size()] ^ (part + 0x3Du + i));
    }
    return material;
}

void IncrementCounter(Block& counter)
{
    for (int i = static_cast<int>(counter.size()) - 1; i >= 0; --i)
    {
        counter[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(counter[static_cast<std::size_t>(i)] + 1u);
        if (counter[static_cast<std::size_t>(i)] != 0)
        {
            break;
        }
    }
}

std::vector<std::uint8_t> ApplyCtr(const std::vector<std::uint8_t>& input,
                                   const mi::shared::crypto::WhiteboxKeyInfo& keyInfo,
                                   bool encrypt)
{
    if (input.empty())
    {
        return {};
    }

    const Block key = DeriveMaterial(keyInfo, 0xC3D2E1F0u);
    const Block iv = DeriveMaterial(keyInfo, 0x1B873593u);

    const std::uint64_t maskSeed = HashKey(keyInfo.keyParts, 0x5EED1234ULL);
    const std::uint64_t encSeed = HashKey(keyInfo.keyParts, 0xABCDEF1122334455ULL);
    WhiteboxTables cipher(key, maskSeed, encSeed);

    Block counter = iv;
    std::vector<std::uint8_t> output(input.size(), 0);

    for (std::size_t offset = 0; offset < input.size(); offset += 16)
    {
        Block keystream = cipher.EncryptBlock(counter);
        const std::size_t chunk = std::min<std::size_t>(16, input.size() - offset);
        for (std::size_t i = 0; i < chunk; ++i)
        {
            output[offset + i] = static_cast<std::uint8_t>(input[offset + i] ^ keystream[i]);
        }
        IncrementCounter(counter);
    }

    // encrypt/decrypt 对称，参数仅用于语义区分
    (void)encrypt;
    return output;
}

std::uint8_t HexToByte(char high, char low)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F')
        {
            return 10 + (c - 'A');
        }
        return -1;
    };

    const int hi = hex(high);
    const int lo = hex(low);
    if (hi < 0 || lo < 0)
    {
        return 0;
    }
    return static_cast<std::uint8_t>((hi << 4) | lo);
}

std::vector<std::uint8_t> ParseHexString(const std::string& text)
{
    std::vector<std::uint8_t> bytes;
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text)
    {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
        {
            filtered.push_back(c);
        }
    }

    if (filtered.size() % 2 != 0)
    {
        filtered.push_back('0');
    }

    for (std::size_t i = 0; i + 1 < filtered.size(); i += 2)
    {
        bytes.push_back(HexToByte(filtered[i], filtered[i + 1]));
    }
    return bytes;
}

std::string BuildEnvName(const std::string& prefix, std::size_t index)
{
    return prefix + std::to_string(index);
}

bool TryGetEnv(const std::string& name, std::string& out)
{
#if defined(_WIN32) && defined(_MSC_VER)
    char* buffer = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buffer, &len, name.c_str()) != 0 || buffer == nullptr)
    {
        return false;
    }
    out.assign(buffer);
    free(buffer);
    return true;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr)
    {
        return false;
    }
    out.assign(value);
    return true;
#endif
}
}  // namespace

namespace mi::shared::crypto
{
std::vector<std::uint8_t> Encrypt(const std::vector<std::uint8_t>& plain, const WhiteboxKeyInfo& keyInfo)
{
    return ApplyCtr(plain, keyInfo, true);
}

std::vector<std::uint8_t> Decrypt(const std::vector<std::uint8_t>& cipher, const WhiteboxKeyInfo& keyInfo)
{
    return ApplyCtr(cipher, keyInfo, false);
}

WhiteboxKeyInfo BuildKeyFromEnv(const std::string& prefix)
{
    WhiteboxKeyInfo key{};
    for (std::size_t i = 0; i < 32; ++i)
    {
        const std::string name = BuildEnvName(prefix, i);
        std::string value;
        if (!TryGetEnv(name, value))
        {
            continue;
        }
        const auto bytes = ParseHexString(value);
        key.keyParts.insert(key.keyParts.end(), bytes.begin(), bytes.end());
    }
    return key;
}

WhiteboxKeyInfo MixKey(const WhiteboxKeyInfo& base, const std::vector<std::uint8_t>& dynamic)
{
    WhiteboxKeyInfo mixed;
    mixed.keyParts = base.keyParts;
    const std::uint64_t seed = HashKey(base.keyParts, 0x7F4A7C159E3779B9ULL);
    for (std::size_t i = 0; i < dynamic.size(); ++i)
    {
        const std::uint8_t derived =
            static_cast<std::uint8_t>(dynamic[i] ^ static_cast<std::uint8_t>((seed >> ((i % 8) * 8)) & 0xFFu) ^
                                      static_cast<std::uint8_t>(0xA5u + static_cast<std::uint8_t>(i * 17u)));
        mixed.keyParts.push_back(derived);
    }
    if (mixed.keyParts.empty())
    {
        mixed.keyParts.push_back(0x5Au);
    }
    return mixed;
}
}  // namespace mi::shared::crypto
