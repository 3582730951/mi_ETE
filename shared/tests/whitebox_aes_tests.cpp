#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "mi/shared/crypto/whitebox_aes.hpp"

int main()
{
    mi::shared::crypto::WhiteboxKeyInfo key{{0x11u, 0x22u, 0x33u, 0x44u}};
    std::vector<std::uint8_t> plain(64);
    for (std::size_t i = 0; i < plain.size(); ++i)
    {
        plain[i] = static_cast<std::uint8_t>(i & 0xFFu);
    }
    const auto cipher = mi::shared::crypto::Encrypt(plain, key);
    const auto restored = mi::shared::crypto::Decrypt(cipher, key);
    assert(plain == restored);
    assert(cipher != plain);

    mi::shared::crypto::WhiteboxKeyInfo anotherKey{{0x10u, 0x22u, 0x35u, 0x44u}};
    const auto cipher2 = mi::shared::crypto::Encrypt(plain, anotherKey);
    assert(cipher2 != cipher);

    // 再次加密应与第一次一致（同一密钥同一输入）
    const auto cipher3 = mi::shared::crypto::Encrypt(plain, key);
    assert(cipher3 == cipher);

    const std::vector<std::uint8_t> dyn{0x01, 0x02, 0x03};
    auto mixedKey = mi::shared::crypto::MixKey(key, dyn);
    const auto mixedCipher = mi::shared::crypto::Encrypt(plain, mixedKey);
    assert(mixedCipher != cipher);
    const auto mixedRestored = mi::shared::crypto::Decrypt(mixedCipher, mixedKey);
    assert(mixedRestored == plain);

    const std::vector<std::uint8_t> empty{};
    const auto cipherEmpty = mi::shared::crypto::Encrypt(empty, key);
    const auto restoredEmpty = mi::shared::crypto::Decrypt(cipherEmpty, key);
    assert(empty == restoredEmpty);

#ifdef _WIN32
    _putenv_s("MI_AES_KEY_PART0", "AA BB CC");
    _putenv_s("MI_AES_KEY_PART1", "dd");
#else
    setenv("MI_AES_KEY_PART0", "AA BB CC", 1);
    setenv("MI_AES_KEY_PART1", "dd", 1);
#endif
    const auto envKey = mi::shared::crypto::BuildKeyFromEnv();
    assert(envKey.keyParts.size() == 4);
    assert(envKey.keyParts[0] == 0xAA);
    assert(envKey.keyParts[1] == 0xBB);
    assert(envKey.keyParts[2] == 0xCC);
    assert(envKey.keyParts[3] == 0xDD);

    return 0;
}
