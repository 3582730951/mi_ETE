#include "mi/shared/storage/chat_history.hpp"

#include <chrono>
#include <cwctype>
#include <iostream>

namespace mi::shared::storage
{
namespace
{
std::uint64_t NowTicks()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
}  // namespace

ChatHistoryStore::ChatHistoryStore(std::filesystem::path rootDirectory, std::vector<std::uint8_t> rootKey)
    : store_(std::move(rootDirectory), std::move(rootKey))
{
}

ChatRecord ChatHistoryStore::Append(std::uint32_t sessionId,
                                    const std::wstring& peer,
                                    const std::vector<std::uint8_t>& payload,
                                    const ChatOptions& options)
{
    ChatRecord rec{};
    rec.sessionId = sessionId;
    rec.peer = peer;
    rec.timestamp = NowTicks();
    rec.format = options.format;
    rec.attachments = options.attachments;
    std::vector<std::uint8_t> content;

    const std::wstring label = BuildName(peer, options.name);
    content.reserve(sizeof(rec.sessionId) + sizeof(rec.timestamp) + payload.size() + 16);

    // 元数据小头序列化
    for (std::size_t i = 0; i < sizeof(rec.sessionId); ++i)
    {
        content.push_back(static_cast<std::uint8_t>((rec.sessionId >> (8 * i)) & 0xFFu));
    }
    for (std::size_t i = 0; i < sizeof(rec.timestamp); ++i)
    {
        content.push_back(static_cast<std::uint8_t>((rec.timestamp >> (8 * i)) & 0xFFu));
    }
    content.push_back(rec.format);
    content.push_back(static_cast<std::uint8_t>(rec.attachments.size()));
    for (const auto& att : rec.attachments)
    {
        const std::string utf8(att.begin(), att.end());
        const std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(utf8.size(), 0xFFFF));
        content.push_back(static_cast<std::uint8_t>(len & 0xFF));
        content.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
        content.insert(content.end(), utf8.begin(), utf8.begin() + len);
    }
    content.insert(content.end(), payload.begin(), payload.end());

    const auto stored = store_.Save(label, content, options.dynamicKey, options.disordered);
    rec.id = stored.id;
    rec.payload = payload;
    return rec;
}

bool ChatHistoryStore::Load(std::uint64_t id, const std::vector<std::uint8_t>& dynamicKey, ChatRecord& out) const
{
    std::vector<std::uint8_t> content;
    if (!store_.Load(id, dynamicKey, content))
    {
        return false;
    }
    if (content.size() < sizeof(out.sessionId) + sizeof(out.timestamp))
    {
        return false;
    }
    std::uint32_t sessionId = 0;
    for (std::size_t i = 0; i < sizeof(sessionId); ++i)
    {
        sessionId |= static_cast<std::uint32_t>(content[i]) << (8 * i);
    }
    std::uint64_t ts = 0;
    for (std::size_t i = 0; i < sizeof(ts); ++i)
    {
        ts |= static_cast<std::uint64_t>(content[sizeof(sessionId) + i]) << (8 * i);
    }

    out.id = id;
    out.sessionId = sessionId;
    out.timestamp = ts;
    std::size_t offset = sizeof(sessionId) + sizeof(ts);
    if (offset < content.size())
    {
        out.format = content[offset];
        offset += 1;
    }
    std::vector<std::wstring> attachments;
    if (offset < content.size())
    {
        const std::uint8_t count = content[offset];
        offset += 1;
        for (std::uint8_t i = 0; i < count && offset + 2 <= content.size(); ++i)
        {
            const std::uint16_t len = static_cast<std::uint16_t>(content[offset] | (static_cast<std::uint16_t>(content[offset + 1]) << 8));
            offset += 2;
            if (offset + len > content.size())
            {
                break;
            }
            std::string utf8(content.begin() + static_cast<long long>(offset),
                             content.begin() + static_cast<long long>(offset + len));
            attachments.push_back(std::wstring(utf8.begin(), utf8.end()));
            offset += len;
        }
    }
    out.attachments = attachments;
    if (offset > content.size())
    {
        return false;
    }
    out.payload.assign(content.begin() + static_cast<long long>(offset), content.end());
    return true;
}

bool ChatHistoryStore::Revoke(std::uint64_t id)
{
    return store_.Revoke(id);
}

bool ChatHistoryStore::Exists(std::uint64_t id) const
{
    return store_.Exists(id);
}

std::wstring ChatHistoryStore::BuildName(const std::wstring& peer, const std::wstring& fallback)
{
    std::wstring sanitized;
    sanitized.reserve(peer.size());
    for (wchar_t c : peer)
    {
        if (std::iswalnum(c) || c == L'.' || c == L'_' || c == L'-')
        {
            sanitized.push_back(c);
        }
    }
    if (sanitized.empty())
    {
        return fallback;
    }
    return sanitized + L".msg";
}
}  // namespace mi::shared::storage
