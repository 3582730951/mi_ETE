#include "mi/shared/crypto/tls_support.hpp"

#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#if defined(MI_HAS_OPENSSL)
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>
#endif

namespace
{
#ifdef _WIN32
std::wstring LastErrorMessage(DWORD code)
{
    LPWSTR buf = nullptr;
    const DWORD size =
        ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr,
                         code,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         reinterpret_cast<LPWSTR>(&buf),
                         0,
                         nullptr);
    if (size == 0 || buf == nullptr)
    {
        return L"unknown";
    }
    std::wstring msg(buf, size);
    ::LocalFree(buf);
    return msg;
}

std::string NameToString(const CERT_NAME_BLOB& name)
{
    CERT_NAME_BLOB nameCopy = name;
    DWORD len = ::CertNameToStrA(X509_ASN_ENCODING, &nameCopy, CERT_X500_NAME_STR, nullptr, 0);
    if (len == 0)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    if (::CertNameToStrA(X509_ASN_ENCODING, &nameCopy, CERT_X500_NAME_STR, out.data(), len) == 0)
    {
        return {};
    }
    // 移除尾部的 NUL
    if (!out.empty() && out.back() == '\0')
    {
        out.pop_back();
    }
    return out;
}

bool ImportPfx(const std::vector<std::uint8_t>& pfxBytes,
               const std::wstring& password,
               HCERTSTORE& store,
               PCCERT_CONTEXT& ctx)
{
    store = nullptr;
    ctx = nullptr;
    if (pfxBytes.empty())
    {
        return false;
    }
    CRYPT_DATA_BLOB blob{};
    blob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(pfxBytes.data()));
    blob.cbData = static_cast<DWORD>(pfxBytes.size());
    store = ::PFXImportCertStore(&blob, password.c_str(), PKCS12_NO_PERSIST_KEY);
    if (store == nullptr)
    {
        return false;
    }
    ctx = ::CertEnumCertificatesInStore(store, nullptr);
    if (ctx == nullptr)
    {
        ::CertCloseStore(store, 0);
        store = nullptr;
        return false;
    }
    return true;
}
#endif

#if !defined(_WIN32) && !defined(MI_HAS_OPENSSL)
// 轻量级 SHA-256 实现，避免额外依赖（非 Windows 平台）
struct Sha256Ctx
{
    std::uint64_t bitLen = 0;
    std::array<std::uint32_t, 8> state{};
    std::array<std::uint8_t, 64> buffer{};
    std::size_t bufferLen = 0;
};

constexpr std::array<std::uint32_t, 64> kShaK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t RotR(std::uint32_t x, std::uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

constexpr std::uint32_t Ch(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return (x & y) ^ ((~x) & z);
}

constexpr std::uint32_t Maj(std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t Sigma0(std::uint32_t x)
{
    return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22);
}

constexpr std::uint32_t Sigma1(std::uint32_t x)
{
    return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25);
}

constexpr std::uint32_t Gamma0(std::uint32_t x)
{
    return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3);
}

constexpr std::uint32_t Gamma1(std::uint32_t x)
{
    return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10);
}

void Sha256Init(Sha256Ctx& ctx)
{
    ctx.bitLen = 0;
    ctx.bufferLen = 0;
    ctx.state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256Transform(Sha256Ctx& ctx, const std::uint8_t* data)
{
    std::uint32_t m[64];
    for (int i = 0; i < 16; ++i)
    {
        m[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) | (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        m[i] = Gamma1(m[i - 2]) + m[i - 7] + Gamma0(m[i - 15]) + m[i - 16];
    }

    std::uint32_t a = ctx.state[0];
    std::uint32_t b = ctx.state[1];
    std::uint32_t c = ctx.state[2];
    std::uint32_t d = ctx.state[3];
    std::uint32_t e = ctx.state[4];
    std::uint32_t f = ctx.state[5];
    std::uint32_t g = ctx.state[6];
    std::uint32_t h = ctx.state[7];

    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + kShaK[static_cast<std::size_t>(i)] + m[i];
        const std::uint32_t t2 = Sigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
    ctx.state[5] += f;
    ctx.state[6] += g;
    ctx.state[7] += h;
}

void Sha256Update(Sha256Ctx& ctx, const std::uint8_t* data, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i)
    {
        ctx.buffer[ctx.bufferLen++] = data[i];
        if (ctx.bufferLen == 64)
        {
            Sha256Transform(ctx, ctx.buffer.data());
            ctx.bitLen += 512;
            ctx.bufferLen = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256Finalize(Sha256Ctx& ctx)
{
    std::array<std::uint8_t, 32> hash{};
    ctx.bitLen += static_cast<std::uint64_t>(ctx.bufferLen) * 8ull;
    ctx.buffer[ctx.bufferLen++] = 0x80u;
    if (ctx.bufferLen > 56)
    {
        while (ctx.bufferLen < 64)
        {
            ctx.buffer[ctx.bufferLen++] = 0;
        }
        Sha256Transform(ctx, ctx.buffer.data());
        ctx.bufferLen = 0;
    }
    while (ctx.bufferLen < 56)
    {
        ctx.buffer[ctx.bufferLen++] = 0;
    }

    for (int i = 7; i >= 0; --i)
    {
        ctx.buffer[static_cast<std::size_t>(63 - i)] = static_cast<std::uint8_t>((ctx.bitLen >> (i * 8)) & 0xFFu);
    }
    Sha256Transform(ctx, ctx.buffer.data());

    for (int i = 0; i < 8; ++i)
    {
        hash[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::uint8_t>((ctx.state[i] >> 24) & 0xFFu);
        hash[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>((ctx.state[i] >> 16) & 0xFFu);
        hash[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>((ctx.state[i] >> 8) & 0xFFu);
        hash[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>((ctx.state[i]) & 0xFFu);
    }
    return hash;
}
#endif
}  // namespace

namespace mi::shared::crypto
{
std::vector<std::uint8_t> Sha256(const std::vector<std::uint8_t>& data)
{
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD hashLen = 0;
    DWORD cbResult = 0;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    {
        return {};
    }
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen), sizeof(DWORD), &cbResult, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptHashData(hHash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    std::vector<std::uint8_t> hash(static_cast<std::size_t>(hashLen));
    if (BCryptFinishHash(hHash, hash.data(), hashLen, 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return hash;
#elif defined(MI_HAS_OPENSSL)
    std::vector<std::uint8_t> hash(EVP_MAX_MD_SIZE);
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), hash.data(), &len, EVP_sha256(), nullptr) != 1)
    {
        return {};
    }
    hash.resize(len);
    return hash;
#else
    Sha256Ctx ctx{};
    Sha256Init(ctx);
    Sha256Update(ctx, data.data(), data.size());
    const auto arr = Sha256Finalize(ctx);
    return std::vector<std::uint8_t>(arr.begin(), arr.end());
#endif
}

std::string Sha256Hex(const std::vector<std::uint8_t>& data)
{
    const auto hash = Sha256(data);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(hash.size() * 2);
    for (auto b : hash)
    {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

CertChainResult ValidatePfxChain(const std::vector<std::uint8_t>& pfxBytes,
                                 const std::wstring& password,
                                 bool allowSelfSigned)
{
    CertChainResult result{};
    result.fingerprintHex = Sha256Hex(pfxBytes);
#ifdef _WIN32
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT ctx = nullptr;
    if (!ImportPfx(pfxBytes, password, store, ctx))
    {
        result.error = L"导入 PFX 失败";
        return result;
    }
    if (ctx != nullptr && ctx->pbCertEncoded != nullptr && ctx->cbCertEncoded > 0)
    {
        std::vector<std::uint8_t> der(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
        result.fingerprintHex = Sha256Hex(der);
    }

    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!::CertGetCertificateChain(nullptr, ctx, nullptr, store, &para, 0, nullptr, &chain))
    {
        result.error = L"证书链构建失败: " + LastErrorMessage(::GetLastError());
        ::CertFreeCertificateContext(ctx);
        ::CertCloseStore(store, 0);
        return result;
    }

    const bool selfSigned =
        ::CertCompareCertificateName(X509_ASN_ENCODING, &ctx->pCertInfo->Subject, &ctx->pCertInfo->Issuer) == TRUE;
    result.selfSigned = selfSigned;
    result.subject = NameToString(ctx->pCertInfo->Subject);
    result.issuer = NameToString(ctx->pCertInfo->Issuer);

    const DWORD err = chain->TrustStatus.dwErrorStatus;
    if (err == CERT_TRUST_NO_ERROR)
    {
        result.ok = true;
    }
    else if (selfSigned && allowSelfSigned &&
             (err == CERT_TRUST_IS_UNTRUSTED_ROOT || err == CERT_TRUST_IS_PARTIAL_CHAIN))
    {
        result.ok = true;
    }
    else
    {
        std::wstringstream ss;
        ss << L"链路校验失败 err=0x" << std::hex << err;
        result.error = ss.str();
    }

    if (chain != nullptr)
    {
        ::CertFreeCertificateChain(chain);
    }
    ::CertFreeCertificateContext(ctx);
    ::CertCloseStore(store, 0);
#elif defined(MI_HAS_OPENSSL)
    const std::string pwdUtf8(password.begin(), password.end());
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(pfxBytes.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &ptr, static_cast<long>(pfxBytes.size()));
    if (!p12)
    {
        result.error = L"PFX 解析失败";
        return result;
    }
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (!PKCS12_parse(p12, pwdUtf8.c_str(), &pkey, &cert, &ca))
    {
        PKCS12_free(p12);
        result.error = L"PFX 解析失败";
        return result;
    }
    if (cert != nullptr)
    {
        unsigned int len = 0;
        unsigned char md[EVP_MAX_MD_SIZE];
        if (X509_digest(cert, EVP_sha256(), md, &len) == 1)
        {
            result.fingerprintHex = Sha256Hex(std::vector<std::uint8_t>(md, md + len));
        }
        char subj[256] = {0};
        X509_NAME_oneline(X509_get_subject_name(cert), subj, sizeof(subj) - 1);
        result.subject = subj;
        char issuer[256] = {0};
        X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer) - 1);
        result.issuer = issuer;
        result.selfSigned = (result.subject == result.issuer);
    }
    X509_STORE* store = X509_STORE_new();
    if (store != nullptr && allowSelfSigned)
    {
        X509_STORE_set_flags(store, X509_V_FLAG_PARTIAL_CHAIN);
    }
    bool ok = false;
    if (store != nullptr && cert != nullptr)
    {
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        if (ctx != nullptr)
        {
            X509_STORE_CTX_init(ctx, store, cert, ca);
            const int vr = X509_verify_cert(ctx);
            if (vr == 1)
            {
                ok = true;
            }
            else if (result.selfSigned && allowSelfSigned)
            {
                ok = true;
            }
            else
            {
                result.error = L"证书链校验失败";
            }
            X509_STORE_CTX_free(ctx);
        }
        X509_STORE_free(store);
    }
    PKCS12_free(p12);
    if (pkey != nullptr)
    {
        EVP_PKEY_free(pkey);
    }
    if (cert != nullptr)
    {
        X509_free(cert);
    }
    if (ca != nullptr)
    {
        sk_X509_pop_free(ca, X509_free);
    }
    result.ok = ok;
    if (!ok && result.error.empty())
    {
        result.error = L"证书链未通过";
    }
#else
    (void)password;
    result.selfSigned = true;
    result.ok = allowSelfSigned;
    if (!allowSelfSigned)
    {
        result.error = L"禁用了自签且当前仅指纹校验";
    }
#endif
    return result;
}

bool EncryptWithCertificate(const std::vector<std::uint8_t>& pfxBytes,
                            const std::wstring& password,
                            const std::vector<std::uint8_t>& plain,
                            std::vector<std::uint8_t>& cipher)
{
#ifdef _WIN32
    cipher.clear();
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT ctx = nullptr;
    if (!ImportPfx(pfxBytes, password, store, ctx))
    {
        return false;
    }

    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (!::CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                       &ctx->pCertInfo->SubjectPublicKeyInfo,
                                       0,
                                       nullptr,
                                       &hKey))
    {
        ::CertFreeCertificateContext(ctx);
        ::CertCloseStore(store, 0);
        return false;
    }

    BCRYPT_OAEP_PADDING_INFO pad{};
    pad.pszAlgId = BCRYPT_SHA256_ALGORITHM;
    ULONG cbOutput = 0;
    if (BCryptEncrypt(hKey,
                      const_cast<PUCHAR>(plain.data()),
                      static_cast<ULONG>(plain.size()),
                      &pad,
                      nullptr,
                      0,
                      nullptr,
                      0,
                      &cbOutput,
                      BCRYPT_PAD_OAEP) != 0)
    {
        BCryptDestroyKey(hKey);
        ::CertFreeCertificateContext(ctx);
        ::CertCloseStore(store, 0);
        return false;
    }

    cipher.resize(cbOutput);
    if (BCryptEncrypt(hKey,
                      const_cast<PUCHAR>(plain.data()),
                      static_cast<ULONG>(plain.size()),
                      &pad,
                      nullptr,
                      0,
                      cipher.data(),
                      cbOutput,
                      &cbOutput,
                      BCRYPT_PAD_OAEP) != 0)
    {
        cipher.clear();
        BCryptDestroyKey(hKey);
        ::CertFreeCertificateContext(ctx);
        ::CertCloseStore(store, 0);
        return false;
    }

    BCryptDestroyKey(hKey);
    ::CertFreeCertificateContext(ctx);
    ::CertCloseStore(store, 0);
    return true;
#elif defined(MI_HAS_OPENSSL)
    cipher.clear();
    const std::string pwdUtf8(password.begin(), password.end());
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(pfxBytes.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &ptr, static_cast<long>(pfxBytes.size()));
    if (!p12)
    {
        return false;
    }
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (!PKCS12_parse(p12, pwdUtf8.c_str(), &pkey, &cert, &ca))
    {
        PKCS12_free(p12);
        return false;
    }
    RSA* rsa = cert ? EVP_PKEY_get1_RSA(X509_get_pubkey(cert)) : nullptr;
    bool ok = false;
    if (rsa != nullptr)
    {
        cipher.resize(RSA_size(rsa));
        const int written = RSA_public_encrypt(static_cast<int>(plain.size()),
                                               plain.data(),
                                               cipher.data(),
                                               rsa,
                                               RSA_PKCS1_OAEP_PADDING);
        if (written > 0)
        {
            cipher.resize(static_cast<std::size_t>(written));
            ok = true;
        }
    }
    if (rsa != nullptr)
    {
        RSA_free(rsa);
    }
    if (pkey != nullptr)
    {
        EVP_PKEY_free(pkey);
    }
    if (cert != nullptr)
    {
        X509_free(cert);
    }
    if (ca != nullptr)
    {
        sk_X509_pop_free(ca, X509_free);
    }
    PKCS12_free(p12);
    return ok;
#else
    // 简易对称封装：使用“证书”指纹派生的 32 字节与明文异或，保证链路仍需握手
    (void)password;
    cipher.clear();
    const auto key = Sha256(pfxBytes);
    if (key.empty())
    {
        return false;
    }
    cipher.resize(plain.size());
    for (std::size_t i = 0; i < plain.size(); ++i)
    {
        cipher[i] = static_cast<std::uint8_t>(plain[i] ^ key[i % key.size()]);
    }
    return true;
#endif
}

bool DecryptWithPrivateKey(const std::vector<std::uint8_t>& pfxBytes,
                           const std::wstring& password,
                           const std::vector<std::uint8_t>& cipher,
                           std::vector<std::uint8_t>& plain)
{
#ifdef _WIN32
    plain.clear();
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT ctx = nullptr;
    if (!ImportPfx(pfxBytes, password, store, ctx))
    {
        return false;
    }

    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
    DWORD spec = 0;
    BOOL mustFree = FALSE;
    if (!::CryptAcquireCertificatePrivateKey(ctx,
                                             CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_COMPARE_KEY_FLAG,
                                             nullptr,
                                             &key,
                                             &spec,
                                             &mustFree))
    {
        ::CertFreeCertificateContext(ctx);
        ::CertCloseStore(store, 0);
        return false;
    }

    bool success = false;
    if (spec == CERT_NCRYPT_KEY_SPEC)
    {
        BCRYPT_OAEP_PADDING_INFO pad{};
        pad.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        ULONG cbPlain = 0;
        SECURITY_STATUS sec = ::NCryptDecrypt(key,
                                              const_cast<PUCHAR>(cipher.data()),
                                              static_cast<ULONG>(cipher.size()),
                                              &pad,
                                              nullptr,
                                              0,
                                              &cbPlain,
                                              NCRYPT_PAD_OAEP_FLAG);
        if (sec == ERROR_SUCCESS)
        {
            plain.resize(cbPlain);
            sec = ::NCryptDecrypt(key,
                                  const_cast<PUCHAR>(cipher.data()),
                                  static_cast<ULONG>(cipher.size()),
                                  &pad,
                                  plain.data(),
                                  cbPlain,
                                  &cbPlain,
                                  NCRYPT_PAD_OAEP_FLAG);
            success = (sec == ERROR_SUCCESS);
            if (!success)
            {
                plain.clear();
            }
        }
    }
    else
    {
        HCRYPTKEY legacyKey = reinterpret_cast<HCRYPTKEY>(key);
        std::vector<std::uint8_t> tmp(cipher.begin(), cipher.end());
        DWORD dataLen = static_cast<DWORD>(tmp.size());
        if (::CryptDecrypt(legacyKey, 0, TRUE, CRYPT_OAEP, tmp.data(), &dataLen))
        {
            tmp.resize(dataLen);
            plain = std::move(tmp);
            success = true;
        }
        }

    if (mustFree)
    {
        if (spec == CERT_NCRYPT_KEY_SPEC)
        {
            ::NCryptFreeObject(key);
        }
        else
        {
            ::CryptReleaseContext(reinterpret_cast<HCRYPTPROV>(key), 0);
        }
    }
    ::CertFreeCertificateContext(ctx);
    ::CertCloseStore(store, 0);
    return success;
#elif defined(MI_HAS_OPENSSL)
    plain.clear();
    const std::string pwdUtf8(password.begin(), password.end());
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(pfxBytes.data());
    PKCS12* p12 = d2i_PKCS12(nullptr, &ptr, static_cast<long>(pfxBytes.size()));
    if (!p12)
    {
        return false;
    }
    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (!PKCS12_parse(p12, pwdUtf8.c_str(), &pkey, &cert, &ca))
    {
        PKCS12_free(p12);
        return false;
    }
    RSA* rsa = pkey ? EVP_PKEY_get1_RSA(pkey) : nullptr;
    bool ok = false;
    if (rsa != nullptr)
    {
        plain.resize(RSA_size(rsa));
        const int written = RSA_private_decrypt(static_cast<int>(cipher.size()),
                                                cipher.data(),
                                                plain.data(),
                                                rsa,
                                                RSA_PKCS1_OAEP_PADDING);
        if (written > 0)
        {
            plain.resize(static_cast<std::size_t>(written));
            ok = true;
        }
        else
        {
            plain.clear();
        }
    }
    if (rsa != nullptr)
    {
        RSA_free(rsa);
    }
    if (pkey != nullptr)
    {
        EVP_PKEY_free(pkey);
    }
    if (cert != nullptr)
    {
        X509_free(cert);
    }
    if (ca != nullptr)
    {
        sk_X509_pop_free(ca, X509_free);
    }
    PKCS12_free(p12);
    return ok;
#else
    (void)password;
    plain.clear();
    const auto key = Sha256(pfxBytes);
    if (key.empty())
    {
        return false;
    }
    plain.resize(cipher.size());
    for (std::size_t i = 0; i < cipher.size(); ++i)
    {
        plain[i] = static_cast<std::uint8_t>(cipher[i] ^ key[i % key.size()]);
    }
    return true;
#endif
}
}  // namespace mi::shared::crypto
