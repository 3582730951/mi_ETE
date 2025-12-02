#include "mi/shared/storage/disordered_file.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
#pragma pack(push, 1)
struct DisorderedHeader
{
    std::uint32_t magic = 0x4D495344;  // "MIDS"
    std::uint16_t version = 1;
    std::uint16_t flags = 0;
    std::uint32_t chunkSize = 0;
    std::uint32_t chunkCount = 0;
    std::uint64_t originalSize = 0;
    std::uint64_t salt = 0;
    std::uint32_t keyDigest = 0;
    std::uint32_t bodyDigest = 0;
};
#pragma pack(pop)

std::uint32_t Fnva(const std::vector<std::uint8_t>& data)
{
    std::uint32_t hash = 2166136261u;
    for (std::uint8_t b : data)
    {
        hash ^= b;
        hash *= 16777619u;
    }
    return hash;
}

template <typename T>
void WriteLe(std::ofstream& stream, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
    {
        const std::uint8_t b = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
        stream.put(static_cast<char>(b));
    }
}

template <typename T>
bool ReadLe(std::ifstream& stream, T& value)
{
    std::array<std::uint8_t, sizeof(T)> buf{};
    stream.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (stream.gcount() != static_cast<std::streamsize>(buf.size()))
    {
        return false;
    }

    T result = 0;
    for (std::size_t i = 0; i < buf.size(); ++i)
    {
        result |= static_cast<T>(buf[i]) << (8 * i);
    }
    value = result;
    return true;
}

std::uint64_t NowTicks()
{
    return static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

std::mt19937_64 BuildEngine(std::uint64_t seed)
{
    if (seed == 0)
    {
        std::random_device rd;
        return std::mt19937_64(static_cast<std::uint64_t>(rd()) ^ NowTicks());
    }
    return std::mt19937_64(seed);
}

std::vector<std::uint8_t> DeriveKeyInternal(const std::vector<std::uint8_t>& root,
                                            const std::vector<std::uint8_t>& dyn,
                                            std::uint64_t salt)
{
    const std::size_t length = 32;
    std::vector<std::uint8_t> key(length, 0);
    std::uint64_t state = salt ^ 0xA5C35A7Bu;

    const auto mix = [&state](std::uint8_t byte, std::size_t i) {
        state ^= static_cast<std::uint64_t>(byte) << ((i % 8) * 8);
        state = (state << 7) | (state >> 57);
        state = state * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL;
    };

    for (std::size_t i = 0; i < root.size(); ++i)
    {
        mix(root[i], i);
    }
    for (std::size_t i = 0; i < dyn.size(); ++i)
    {
        mix(dyn[i], i + root.size());
    }

    for (std::size_t i = 0; i < length; ++i)
    {
        state ^= (state >> 11) ^ (state << 17) ^ static_cast<std::uint64_t>(i * 131u);
        key[i] = static_cast<std::uint8_t>((state >> ((i % 8) * 8)) & 0xFFu);
    }

    if (std::all_of(key.begin(), key.end(), [](std::uint8_t v) { return v == 0; }))
    {
        for (std::size_t i = 0; i < key.size(); ++i)
        {
            key[i] = static_cast<std::uint8_t>((salt >> (i % 8)) & 0xFFu);
        }
    }
    return key;
}

void ApplyMask(std::vector<std::uint8_t>& buffer,
               const std::vector<std::uint8_t>& key,
               std::uint32_t chunkIndex)
{
    if (key.empty())
    {
        return;
    }

    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        const std::uint8_t k = key[i % key.size()];
        const std::uint8_t mix = static_cast<std::uint8_t>((chunkIndex * 31u + i * 17u) & 0xFFu);
        buffer[i] = static_cast<std::uint8_t>(buffer[i] ^ k ^ mix);
    }
}

bool SecureErase(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
    {
        return false;
    }

    const std::uintmax_t size = std::filesystem::file_size(path);
    std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    std::vector<char> zeros(4096, 0);
    std::vector<char> randoms(4096, 0);
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);

    // 覆盖随机数据与零数据
    for (char& c : randoms)
    {
        c = static_cast<char>(dist(gen));
    }

    std::uintmax_t remaining = size;
    while (remaining > 0)
    {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, zeros.size()));
        stream.write(zeros.data(), static_cast<std::streamsize>(chunk));
        stream.seekp(-static_cast<std::streamoff>(chunk), std::ios::cur);
        stream.write(randoms.data(), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
    }

    stream.close();
    std::filesystem::remove(path);
    return true;
}
}  // namespace

namespace mi::shared::storage
{
DisorderedFileStore::DisorderedFileStore(std::filesystem::path rootDirectory, std::vector<std::uint8_t> rootKey)
    : rootDirectory_(std::move(rootDirectory)), rootKey_(std::move(rootKey))
{
    std::filesystem::create_directories(rootDirectory_);
}

StoredFile DisorderedFileStore::Save(const std::wstring& name,
                                     const std::vector<std::uint8_t>& content,
                                     const std::vector<std::uint8_t>& dynamicKey,
                                     const DisorderedOptions& options)
{
    const std::uint32_t chunkSize = options.chunkSize == 0 ? 4096u : options.chunkSize;
    const std::uint32_t chunkCount = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>((content.size() + chunkSize - 1u) / chunkSize));

    const std::uint64_t salt = options.seed == 0 ? NowTicks() : options.seed;
    const auto derivedKey = DeriveKey(dynamicKey, salt);
    const std::uint32_t keyDigest = Fnva(derivedKey);

    std::vector<std::vector<std::uint8_t>> chunks;
    chunks.reserve(chunkCount);
    for (std::uint32_t i = 0; i < chunkCount; ++i)
    {
        const std::size_t offset = static_cast<std::size_t>(i) * static_cast<std::size_t>(chunkSize);
        const std::size_t available = std::min<std::size_t>(chunkSize, content.size() - offset);
        std::vector<std::uint8_t> chunk(chunkSize, 0);
        if (available > 0)
        {
            std::copy_n(content.begin() + static_cast<long long>(offset),
                        static_cast<long long>(available),
                        chunk.begin());
        }
        ApplyMask(chunk, derivedKey, i);
        chunks.push_back(std::move(chunk));
    }

    std::vector<std::uint32_t> permutation(chunkCount);
    std::iota(permutation.begin(), permutation.end(), 0u);
    std::shuffle(permutation.begin(), permutation.end(), BuildEngine(options.seed ^ salt ^ keyDigest));

    std::vector<std::uint8_t> body;
    body.reserve(static_cast<std::size_t>(chunkCount) * static_cast<std::size_t>(chunkSize));
    for (std::uint32_t storedIndex = 0; storedIndex < chunkCount; ++storedIndex)
    {
        const std::uint32_t originalIndex = permutation[storedIndex];
        const auto& chunk = chunks[originalIndex];
        body.insert(body.end(), chunk.begin(), chunk.end());
    }

    DisorderedHeader header{};
    header.chunkSize = chunkSize;
    header.chunkCount = chunkCount;
    header.originalSize = static_cast<std::uint64_t>(content.size());
    header.salt = salt;
    header.keyDigest = keyDigest;
    header.bodyDigest = Fnva(body);

    const std::uint64_t id = static_cast<std::uint64_t>(salt ^ (static_cast<std::uint64_t>(body.size()) << 8));
    const std::filesystem::path path = ResolvePath(id, name);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        throw std::runtime_error("Failed to open file for disordered save");
    }

    stream.write(reinterpret_cast<const char*>(&header), static_cast<std::streamsize>(sizeof(header)));
    for (std::uint32_t index : permutation)
    {
        WriteLe<std::uint32_t>(stream, index);
    }
    stream.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    stream.flush();

    StoredFile result{};
    result.id = id;
    result.path = path;
    result.originalSize = header.originalSize;
    return result;
}

bool DisorderedFileStore::Load(std::uint64_t id,
                               const std::vector<std::uint8_t>& dynamicKey,
                               std::vector<std::uint8_t>& outContent) const
{
    const std::filesystem::path path = ResolvePath(id, L"");
    if (!std::filesystem::exists(path))
    {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    DisorderedHeader header{};
    stream.read(reinterpret_cast<char*>(&header), static_cast<std::streamsize>(sizeof(header)));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header)))
    {
        return false;
    }

    if (header.magic != 0x4D495344 || header.version != 1 || header.chunkSize == 0 || header.chunkCount == 0)
    {
        return false;
    }

    std::vector<std::uint32_t> permutation(header.chunkCount, 0);
    for (std::uint32_t i = 0; i < header.chunkCount; ++i)
    {
        if (!ReadLe<std::uint32_t>(stream, permutation[i]))
        {
            return false;
        }
    }

    const auto derivedKey = DeriveKey(dynamicKey, header.salt);
    if (Fnva(derivedKey) != header.keyDigest)
    {
        return false;
    }

    std::vector<std::uint8_t> body(static_cast<std::size_t>(header.chunkSize) *
                                   static_cast<std::size_t>(header.chunkCount));
    stream.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()));
    if (stream.gcount() != static_cast<std::streamsize>(body.size()))
    {
        return false;
    }

    if (Fnva(body) != header.bodyDigest)
    {
        return false;
    }

    std::vector<std::vector<std::uint8_t>> chunks(header.chunkCount);
    for (std::uint32_t storedIndex = 0; storedIndex < header.chunkCount; ++storedIndex)
    {
        const std::uint32_t originalIndex = permutation[storedIndex];
        const std::size_t offset = static_cast<std::size_t>(storedIndex) *
                                   static_cast<std::size_t>(header.chunkSize);
        chunks[originalIndex] = std::vector<std::uint8_t>(
            body.begin() + static_cast<long long>(offset),
            body.begin() + static_cast<long long>(offset + header.chunkSize));
        ApplyMask(chunks[originalIndex], derivedKey, originalIndex);
    }

    outContent.clear();
    outContent.reserve(static_cast<std::size_t>(header.originalSize));
    for (std::uint32_t i = 0; i < header.chunkCount; ++i)
    {
        const auto& chunk = chunks[i];
        const std::size_t expected = static_cast<std::size_t>(header.originalSize - outContent.size());
        const std::size_t copySize = std::min<std::size_t>(expected, chunk.size());
        outContent.insert(outContent.end(), chunk.begin(), chunk.begin() + static_cast<long long>(copySize));
        if (outContent.size() >= header.originalSize)
        {
            break;
        }
    }

    return outContent.size() == header.originalSize;
}

bool DisorderedFileStore::Revoke(std::uint64_t id)
{
    const std::filesystem::path path = ResolvePath(id, L"");
    if (!std::filesystem::exists(path))
    {
        return false;
    }
    return SecureErase(path);
}

bool DisorderedFileStore::Exists(std::uint64_t id) const
{
    const std::filesystem::path path = ResolvePath(id, L"");
    return std::filesystem::exists(path);
}

bool DisorderedFileStore::IsSupportedMediaExtension(const std::wstring& name)
{
    const std::array<std::wstring, 12> exts = {L".png",  L".jpg",  L".jpeg", L".bmp",  L".gif",  L".webp",
                                               L".tiff", L".mp4",  L".mov",  L".mkv",  L".avi",  L".heic"};

    const auto pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos)
    {
        return false;
    }
    std::wstring ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });

    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

std::filesystem::path DisorderedFileStore::ResolvePath(std::uint64_t id, const std::wstring& name) const
{
    const std::wstring prefix = L"artifact_" + std::to_wstring(id);
    if (name.empty())
    {
        for (const auto& entry : std::filesystem::directory_iterator(rootDirectory_))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::wstring fname = entry.path().filename().wstring();
            if (fname.find(prefix) == 0)
            {
                return entry.path();
            }
        }
        return rootDirectory_ / (prefix + L".mids");
    }

    std::wstring extension = L".mids";
    if (IsSupportedMediaExtension(name))
    {
        const auto pos = name.find_last_of(L'.');
        extension = name.substr(pos);
    }

    return rootDirectory_ / (prefix + extension);
}

std::vector<std::uint8_t> DisorderedFileStore::DeriveKey(const std::vector<std::uint8_t>& dynamicKey,
                                                         std::uint64_t salt) const
{
    return DeriveKeyInternal(rootKey_, dynamicKey, salt);
}
}  // namespace mi::shared::storage
