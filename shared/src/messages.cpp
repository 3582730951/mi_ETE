#include "mi/shared/proto/messages.hpp"

#include <algorithm>
#include <codecvt>
#include <locale>
#include <string>
#include <vector>

namespace
{
template <typename T>
void WriteLe(std::vector<std::uint8_t>& out, T value)
{
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

template <typename T>
bool ReadLe(const std::vector<std::uint8_t>& data, size_t& offset, T& value)
{
    if (offset + sizeof(T) > data.size())
    {
        return false;
    }

    T result = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        result |= (static_cast<T>(data[offset + i]) << (8 * i));
    }
    offset += sizeof(T);
    value = result;
    return true;
}

std::wstring Utf8ToWide(const std::string& text)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(text);
}

std::string WideToUtf8(const std::wstring& text)
{
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.to_bytes(text);
}
}  // namespace

namespace mi::shared::proto
{
std::vector<std::uint8_t> SerializeAuthRequest(const AuthRequest& req)
{
    std::vector<std::uint8_t> buffer;
    const std::string user = WideToUtf8(req.username);
    const std::string pass = WideToUtf8(req.password);

    WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(user.size()));
    buffer.insert(buffer.end(), user.begin(), user.end());
    WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(pass.size()));
    buffer.insert(buffer.end(), pass.begin(), pass.end());
    return buffer;
}

bool ParseAuthRequest(const std::vector<std::uint8_t>& buffer, AuthRequest& out)
{
    size_t offset = 0;
    std::uint16_t userLen = 0;
    if (!ReadLe<std::uint16_t>(buffer, offset, userLen))
    {
        return false;
    }

    if (offset + userLen > buffer.size())
    {
        return false;
    }

    std::string user(buffer.begin() + static_cast<long long>(offset),
                     buffer.begin() + static_cast<long long>(offset + userLen));
    offset += userLen;

    std::uint16_t passLen = 0;
    if (!ReadLe<std::uint16_t>(buffer, offset, passLen))
    {
        return false;
    }

    if (offset + passLen > buffer.size())
    {
        return false;
    }

    std::string pass(buffer.begin() + static_cast<long long>(offset),
                     buffer.begin() + static_cast<long long>(offset + passLen));

    out.username = Utf8ToWide(user);
    out.password = Utf8ToWide(pass);
    return true;
}

std::vector<std::uint8_t> SerializeAuthResponse(const AuthResponse& resp)
{
    std::vector<std::uint8_t> buffer;
    buffer.push_back(resp.success ? 1u : 0u);
    WriteLe<std::uint32_t>(buffer, resp.sessionId);
    return buffer;
}

bool ParseAuthResponse(const std::vector<std::uint8_t>& buffer, AuthResponse& out)
{
    size_t offset = 0;
    if (buffer.empty())
    {
        return false;
    }

    out.success = buffer[0] != 0;
    offset += 1;

    std::uint32_t sessionId = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, sessionId))
    {
        return false;
    }

    out.sessionId = sessionId;
    return true;
}

std::vector<std::uint8_t> SerializeDataPacket(const DataPacket& packet)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, packet.sessionId);
    WriteLe<std::uint32_t>(buffer, packet.targetSessionId);
    WriteLe<std::uint32_t>(buffer, static_cast<std::uint32_t>(packet.payload.size()));
    buffer.insert(buffer.end(), packet.payload.begin(), packet.payload.end());
    return buffer;
}

bool ParseDataPacket(const std::vector<std::uint8_t>& buffer, DataPacket& out)
{
    size_t offset = 0;
    std::uint32_t sessionId = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, sessionId))
    {
        return false;
    }

    std::uint32_t targetId = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, targetId))
    {
        return false;
    }

    std::uint32_t payloadLen = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, payloadLen))
    {
        return false;
    }

    if (offset + payloadLen > buffer.size())
    {
        return false;
    }

    out.sessionId = sessionId;
    out.targetSessionId = targetId;
    out.payload.assign(buffer.begin() + static_cast<long long>(offset),
                       buffer.begin() + static_cast<long long>(offset + payloadLen));
    return true;
}

std::vector<std::uint8_t> SerializeErrorResponse(const ErrorResponse& error)
{
    std::vector<std::uint8_t> buffer;
    buffer.push_back(error.code);
    buffer.push_back(error.severity);
    WriteLe<std::uint32_t>(buffer, error.retryAfterMs);
    const std::string utf8 = WideToUtf8(error.message);
    WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(utf8.size()));
    buffer.insert(buffer.end(), utf8.begin(), utf8.end());
    return buffer;
}

bool ParseErrorResponse(const std::vector<std::uint8_t>& buffer, ErrorResponse& out)
{
    size_t offset = 0;
    if (buffer.size() < 6)
    {
        return false;
    }
    out.code = buffer[offset++];
    out.severity = buffer[offset++];
    if (!ReadLe<std::uint32_t>(buffer, offset, out.retryAfterMs))
    {
        return false;
    }
    std::uint16_t messageLen = 0;
    if (!ReadLe<std::uint16_t>(buffer, offset, messageLen))
    {
        return false;
    }
    if (offset + messageLen > buffer.size())
    {
        return false;
    }
    const std::string text(buffer.begin() + static_cast<long long>(offset),
                           buffer.begin() + static_cast<long long>(offset + messageLen));
    out.message = Utf8ToWide(text);
    return true;
}

std::vector<std::uint8_t> SerializeMediaChunk(const MediaChunk& media)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, media.sessionId);
    WriteLe<std::uint32_t>(buffer, media.targetSessionId);
    WriteLe<std::uint64_t>(buffer, media.mediaId);
    WriteLe<std::uint32_t>(buffer, media.chunkIndex);
    WriteLe<std::uint32_t>(buffer, media.totalChunks);
    WriteLe<std::uint32_t>(buffer, media.totalSize);
    const std::string utf8 = WideToUtf8(media.name);
    WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(utf8.size()));
    buffer.insert(buffer.end(), utf8.begin(), utf8.end());
    WriteLe<std::uint32_t>(buffer, static_cast<std::uint32_t>(media.payload.size()));
    buffer.insert(buffer.end(), media.payload.begin(), media.payload.end());
    return buffer;
}

bool ParseMediaChunk(const std::vector<std::uint8_t>& buffer, MediaChunk& out)
{
    size_t offset = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.targetSessionId) ||
        !ReadLe<std::uint64_t>(buffer, offset, out.mediaId) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.chunkIndex) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.totalChunks) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.totalSize))
    {
        return false;
    }

    std::uint16_t nameLen = 0;
    if (!ReadLe<std::uint16_t>(buffer, offset, nameLen))
    {
        return false;
    }
    if (offset + nameLen > buffer.size())
    {
        return false;
    }

    std::string utf8(buffer.begin() + static_cast<long long>(offset),
                     buffer.begin() + static_cast<long long>(offset + nameLen));
    offset += nameLen;

    std::uint32_t payloadLen = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, payloadLen))
    {
        return false;
    }
    if (offset + payloadLen > buffer.size())
    {
        return false;
    }

    out.name = Utf8ToWide(utf8);
    out.payload.assign(buffer.begin() + static_cast<long long>(offset),
                       buffer.begin() + static_cast<long long>(offset + payloadLen));
    return true;
}

std::vector<std::uint8_t> SerializeMediaControl(const MediaControl& ctl)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, ctl.sessionId);
    WriteLe<std::uint32_t>(buffer, ctl.targetSessionId);
    WriteLe<std::uint64_t>(buffer, ctl.mediaId);
    buffer.push_back(ctl.action);
    return buffer;
}

bool ParseMediaControl(const std::vector<std::uint8_t>& buffer, MediaControl& out)
{
    size_t offset = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.targetSessionId) ||
        !ReadLe<std::uint64_t>(buffer, offset, out.mediaId))
    {
        return false;
    }
    if (offset >= buffer.size())
    {
        return false;
    }
    out.action = buffer[offset];
    return true;
}

std::vector<std::uint8_t> SerializeChatMessage(const ChatMessage& msg)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, msg.sessionId);
    WriteLe<std::uint32_t>(buffer, msg.targetSessionId);
    WriteLe<std::uint64_t>(buffer, msg.messageId);
    buffer.push_back(msg.format);
    WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(msg.attachments.size()));
    for (const auto& name : msg.attachments)
    {
        const std::string utf8 = WideToUtf8(name);
        WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(utf8.size()));
        buffer.insert(buffer.end(), utf8.begin(), utf8.end());
    }
    WriteLe<std::uint32_t>(buffer, static_cast<std::uint32_t>(msg.payload.size()));
    buffer.insert(buffer.end(), msg.payload.begin(), msg.payload.end());
    return buffer;
}

bool ParseChatMessage(const std::vector<std::uint8_t>& buffer, ChatMessage& out)
{
    size_t offset = 0;
    std::uint32_t payloadSize = 0;
    std::uint16_t attachmentCount = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.targetSessionId) ||
        !ReadLe<std::uint64_t>(buffer, offset, out.messageId))
    {
        return false;
    }
    if (offset >= buffer.size())
    {
        return false;
    }
    out.format = buffer[offset++];
    if (!ReadLe<std::uint16_t>(buffer, offset, attachmentCount))
    {
        return false;
    }
    out.attachments.clear();
    for (std::uint16_t i = 0; i < attachmentCount; ++i)
    {
        std::uint16_t len = 0;
        if (!ReadLe<std::uint16_t>(buffer, offset, len))
        {
            return false;
        }
        if (offset + len > buffer.size())
        {
            return false;
        }
        const std::string utf8(buffer.begin() + static_cast<long long>(offset),
                               buffer.begin() + static_cast<long long>(offset + len));
        out.attachments.push_back(Utf8ToWide(utf8));
        offset += len;
    }
    if (!ReadLe<std::uint32_t>(buffer, offset, payloadSize))
    {
        return false;
    }
    if (offset + payloadSize > buffer.size())
    {
        return false;
    }
    out.payload.assign(buffer.begin() + static_cast<long long>(offset),
                       buffer.begin() + static_cast<long long>(offset + payloadSize));
    return true;
}

std::vector<std::uint8_t> SerializeChatControl(const ChatControl& ctl)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, ctl.sessionId);
    WriteLe<std::uint32_t>(buffer, ctl.targetSessionId);
    WriteLe<std::uint64_t>(buffer, ctl.messageId);
    buffer.push_back(ctl.action);
    return buffer;
}

bool ParseChatControl(const std::vector<std::uint8_t>& buffer, ChatControl& out)
{
    size_t offset = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId) ||
        !ReadLe<std::uint32_t>(buffer, offset, out.targetSessionId) ||
        !ReadLe<std::uint64_t>(buffer, offset, out.messageId))
    {
        return false;
    }
    if (offset >= buffer.size())
    {
        return false;
    }
    out.action = buffer[offset];
    return true;
}

std::vector<std::uint8_t> SerializeSessionListRequest(const SessionListRequest& req)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, req.sessionId);
    buffer.push_back(req.subscribe ? 1u : 0u);
    return buffer;
}

bool ParseSessionListRequest(const std::vector<std::uint8_t>& buffer, SessionListRequest& out)
{
    size_t offset = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId))
    {
        return false;
    }
    if (offset >= buffer.size())
    {
        return false;
    }
    out.subscribe = buffer[offset] != 0;
    return true;
}

std::vector<std::uint8_t> SerializeSessionListResponse(const SessionListResponse& resp)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, static_cast<std::uint32_t>(resp.sessions.size()));
    buffer.push_back(resp.subscribed ? 1u : 0u);
    WriteLe<std::uint32_t>(buffer, resp.serverTimeSec);
    for (const auto& item : resp.sessions)
    {
        WriteLe<std::uint32_t>(buffer, item.sessionId);
        WriteLe<std::uint32_t>(buffer, item.unreadCount);
        const std::string utf8 = WideToUtf8(item.peer);
        WriteLe<std::uint16_t>(buffer, static_cast<std::uint16_t>(utf8.size()));
        buffer.insert(buffer.end(), utf8.begin(), utf8.end());
    }
    return buffer;
}

bool ParseSessionListResponse(const std::vector<std::uint8_t>& buffer, SessionListResponse& out)
{
    size_t offset = 0;
    std::uint32_t count = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, count))
    {
        return false;
    }
    if (offset >= buffer.size())
    {
        return false;
    }
    out.subscribed = buffer[offset] != 0;
    offset += 1;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.serverTimeSec))
    {
        return false;
    }

    out.sessions.clear();
    out.sessions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        SessionInfo info{};
        if (!ReadLe<std::uint32_t>(buffer, offset, info.sessionId))
        {
            return false;
        }
        if (!ReadLe<std::uint32_t>(buffer, offset, info.unreadCount))
        {
            return false;
        }
        std::uint16_t peerLen = 0;
        if (!ReadLe<std::uint16_t>(buffer, offset, peerLen))
        {
            return false;
        }
        if (offset + peerLen > buffer.size())
        {
            return false;
        }
        std::string utf8(buffer.begin() + static_cast<long long>(offset),
                         buffer.begin() + static_cast<long long>(offset + peerLen));
        offset += peerLen;
        info.peer = Utf8ToWide(utf8);
        out.sessions.push_back(std::move(info));
    }
    return true;
}

std::vector<std::uint8_t> SerializeStatsReport(const StatsReport& rpt)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, rpt.sessionId);
    WriteLe<std::uint64_t>(buffer, rpt.bytesSent);
    WriteLe<std::uint64_t>(buffer, rpt.bytesReceived);
    WriteLe<std::uint32_t>(buffer, rpt.chatFailures);
    WriteLe<std::uint32_t>(buffer, rpt.dataFailures);
    WriteLe<std::uint32_t>(buffer, rpt.mediaFailures);
    WriteLe<std::uint32_t>(buffer, rpt.durationMs);
    return buffer;
}

bool ParseStatsReport(const std::vector<std::uint8_t>& buffer, StatsReport& out)
{
    size_t offset = 0;
    const bool ok = ReadLe<std::uint32_t>(buffer, offset, out.sessionId) &&
                    ReadLe<std::uint64_t>(buffer, offset, out.bytesSent) &&
                    ReadLe<std::uint64_t>(buffer, offset, out.bytesReceived) &&
                    ReadLe<std::uint32_t>(buffer, offset, out.chatFailures) &&
                    ReadLe<std::uint32_t>(buffer, offset, out.dataFailures) &&
                    ReadLe<std::uint32_t>(buffer, offset, out.mediaFailures);
    if (!ok)
    {
        return false;
    }
    if (offset + sizeof(std::uint32_t) <= buffer.size())
    {
        if (!ReadLe<std::uint32_t>(buffer, offset, out.durationMs))
        {
            out.durationMs = 0;
        }
    }
    else
    {
        out.durationMs = 0;
    }
    return true;
}

std::vector<std::uint8_t> SerializeStatsHistoryRequest(const StatsHistoryRequest& req)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, req.sessionId);
    return buffer;
}

bool ParseStatsHistoryRequest(const std::vector<std::uint8_t>& buffer, StatsHistoryRequest& out)
{
    size_t offset = 0;
    return ReadLe<std::uint32_t>(buffer, offset, out.sessionId);
}

std::vector<std::uint8_t> SerializeStatsHistoryResponse(const StatsHistoryResponse& resp)
{
    std::vector<std::uint8_t> buffer;
    WriteLe<std::uint32_t>(buffer, resp.sessionId);
    WriteLe<std::uint32_t>(buffer, static_cast<std::uint32_t>(resp.samples.size()));
    for (const auto& sample : resp.samples)
    {
        WriteLe<std::uint32_t>(buffer, sample.sessionId);
        WriteLe<std::uint32_t>(buffer, sample.timestampSec);
        const auto statsBuf = SerializeStatsReport(sample.stats);
        buffer.insert(buffer.end(), statsBuf.begin(), statsBuf.end());
    }
    return buffer;
}

bool ParseStatsHistoryResponse(const std::vector<std::uint8_t>& buffer, StatsHistoryResponse& out)
{
    size_t offset = 0;
    std::uint32_t count = 0;
    if (!ReadLe<std::uint32_t>(buffer, offset, out.sessionId) ||
        !ReadLe<std::uint32_t>(buffer, offset, count))
    {
        return false;
    }
    out.samples.clear();
    out.samples.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        StatsSample sample{};
        if (!ReadLe<std::uint32_t>(buffer, offset, sample.sessionId) ||
            !ReadLe<std::uint32_t>(buffer, offset, sample.timestampSec))
        {
            return false;
        }
        if (!ReadLe<std::uint32_t>(buffer, offset, sample.stats.sessionId) ||
            !ReadLe<std::uint64_t>(buffer, offset, sample.stats.bytesSent) ||
            !ReadLe<std::uint64_t>(buffer, offset, sample.stats.bytesReceived) ||
            !ReadLe<std::uint32_t>(buffer, offset, sample.stats.chatFailures) ||
            !ReadLe<std::uint32_t>(buffer, offset, sample.stats.dataFailures) ||
            !ReadLe<std::uint32_t>(buffer, offset, sample.stats.mediaFailures))
        {
            return false;
        }
        if (!ReadLe<std::uint32_t>(buffer, offset, sample.stats.durationMs))
        {
            sample.stats.durationMs = 0;
        }
        out.samples.push_back(sample);
    }
    return true;
}
}  // namespace mi::shared::proto
