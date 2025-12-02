#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "mi/shared/storage/disordered_file.hpp"

namespace mi::shared::storage
{
struct ChatRecord
{
    std::uint64_t id = 0;
    std::uint32_t sessionId = 0;
    std::wstring peer;
    std::uint64_t timestamp = 0;
    std::uint8_t format = 0;  // 0 plain, 1 markdown/html
    std::vector<std::uint8_t> payload;
    std::vector<std::wstring> attachments;
};

struct ChatOptions
{
    std::vector<std::uint8_t> dynamicKey;
    std::wstring name = L"chat.msg";
    DisorderedOptions disordered{};
    std::uint8_t format = 0;
    std::vector<std::wstring> attachments;
};

class ChatHistoryStore
{
public:
    ChatHistoryStore(std::filesystem::path rootDirectory, std::vector<std::uint8_t> rootKey);

    // 追加聊天记录并落盘（乱序加密），返回记录 id。
    ChatRecord Append(std::uint32_t sessionId,
                      const std::wstring& peer,
                      const std::vector<std::uint8_t>& payload,
                      const ChatOptions& options = {});

    // 读取记录，如果不存在或密钥不匹配返回 false。
    bool Load(std::uint64_t id, const std::vector<std::uint8_t>& dynamicKey, ChatRecord& out) const;

    bool Revoke(std::uint64_t id);
    bool Exists(std::uint64_t id) const;

private:
    static std::wstring BuildName(const std::wstring& peer, const std::wstring& fallback);

    DisorderedFileStore store_;
};
}  // namespace mi::shared::storage
