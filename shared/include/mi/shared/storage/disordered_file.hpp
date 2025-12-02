#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mi::shared::storage
{
struct DisorderedOptions
{
    std::uint32_t chunkSize = 4096;
    std::uint64_t seed = 0;  // 0 表示使用随机种子
};

struct StoredFile
{
    std::uint64_t id = 0;
    std::filesystem::path path;
    std::uint64_t originalSize = 0;
};

class DisorderedFileStore
{
public:
    DisorderedFileStore(std::filesystem::path rootDirectory, std::vector<std::uint8_t> rootKey = {});

    StoredFile Save(const std::wstring& name,
                    const std::vector<std::uint8_t>& content,
                    const std::vector<std::uint8_t>& dynamicKey,
                    const DisorderedOptions& options = {});

    bool Load(std::uint64_t id,
              const std::vector<std::uint8_t>& dynamicKey,
              std::vector<std::uint8_t>& outContent) const;

    bool Revoke(std::uint64_t id);
    bool Exists(std::uint64_t id) const;

    static bool IsSupportedMediaExtension(const std::wstring& name);

private:
    std::filesystem::path ResolvePath(std::uint64_t id, const std::wstring& name) const;
    std::vector<std::uint8_t> DeriveKey(const std::vector<std::uint8_t>& dynamicKey,
                                        std::uint64_t salt) const;

    std::filesystem::path rootDirectory_;
    std::vector<std::uint8_t> rootKey_;
};
}  // namespace mi::shared::storage
