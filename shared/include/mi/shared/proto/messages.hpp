#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mi::shared::proto
{
struct AuthRequest
{
    std::wstring username;
    std::wstring password;
};

struct AuthResponse
{
    bool success = false;
    std::uint32_t sessionId = 0;
};

struct DataPacket
{
    std::uint32_t sessionId = 0;
    std::uint32_t targetSessionId = 0;
    std::vector<std::uint8_t> payload;
};

struct ErrorResponse
{
    std::uint8_t code = 0;
    std::uint8_t severity = 0;  // 0: info/warn, 1: retryable, 2: fatal
    std::uint32_t retryAfterMs = 0;
    std::wstring message;
};

struct MediaChunk
{
    std::uint32_t sessionId = 0;
    std::uint32_t targetSessionId = 0;
    std::uint64_t mediaId = 0;
    std::uint32_t chunkIndex = 0;
    std::uint32_t totalChunks = 0;
    std::uint32_t totalSize = 0;
    std::wstring name;
    std::vector<std::uint8_t> payload;
};

struct MediaControl
{
    std::uint32_t sessionId = 0;
    std::uint32_t targetSessionId = 0;
    std::uint64_t mediaId = 0;
    std::uint8_t action = 0;  // 1: revoke
};

struct ChatMessage
{
    std::uint32_t sessionId = 0;
    std::uint32_t targetSessionId = 0;
    std::uint64_t messageId = 0;
    std::uint8_t format = 0;  // 0: plain, 1: markdown/html
    std::vector<std::wstring> attachments;  // 文件/媒体名列表，便于 UI 展示
    std::vector<std::uint8_t> payload;
};

struct ChatControl
{
    std::uint32_t sessionId = 0;
    std::uint32_t targetSessionId = 0;
    std::uint64_t messageId = 0;
    std::uint8_t action = 0;  // 1: revoke
};

struct SessionListRequest
{
    std::uint32_t sessionId = 0;
    bool subscribe = false;
};

struct SessionInfo
{
    std::uint32_t sessionId = 0;
    std::wstring peer;
    std::uint32_t unreadCount = 0;
};

struct SessionListResponse
{
    std::vector<SessionInfo> sessions;
    bool subscribed = false;
    std::uint32_t serverTimeSec = 0;
};

struct StatsReport
{
    std::uint32_t sessionId = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint32_t chatFailures = 0;
    std::uint32_t dataFailures = 0;
    std::uint32_t mediaFailures = 0;
    std::uint32_t durationMs = 0;
};

struct StatsSample
{
    std::uint32_t sessionId = 0;
    std::uint32_t timestampSec = 0;
    StatsReport stats;
};

struct StatsHistoryRequest
{
    std::uint32_t sessionId = 0;
};

struct StatsHistoryResponse
{
    std::uint32_t sessionId = 0;
    std::vector<StatsSample> samples;
};

std::vector<std::uint8_t> SerializeAuthRequest(const AuthRequest& req);
bool ParseAuthRequest(const std::vector<std::uint8_t>& buffer, AuthRequest& out);

std::vector<std::uint8_t> SerializeAuthResponse(const AuthResponse& resp);
bool ParseAuthResponse(const std::vector<std::uint8_t>& buffer, AuthResponse& out);

std::vector<std::uint8_t> SerializeDataPacket(const DataPacket& packet);
bool ParseDataPacket(const std::vector<std::uint8_t>& buffer, DataPacket& out);

std::vector<std::uint8_t> SerializeErrorResponse(const ErrorResponse& error);
bool ParseErrorResponse(const std::vector<std::uint8_t>& buffer, ErrorResponse& out);

std::vector<std::uint8_t> SerializeMediaChunk(const MediaChunk& media);
bool ParseMediaChunk(const std::vector<std::uint8_t>& buffer, MediaChunk& out);

std::vector<std::uint8_t> SerializeMediaControl(const MediaControl& ctl);
bool ParseMediaControl(const std::vector<std::uint8_t>& buffer, MediaControl& out);

std::vector<std::uint8_t> SerializeChatMessage(const ChatMessage& msg);
bool ParseChatMessage(const std::vector<std::uint8_t>& buffer, ChatMessage& out);

std::vector<std::uint8_t> SerializeChatControl(const ChatControl& ctl);
bool ParseChatControl(const std::vector<std::uint8_t>& buffer, ChatControl& out);

std::vector<std::uint8_t> SerializeSessionListRequest(const SessionListRequest& req);
bool ParseSessionListRequest(const std::vector<std::uint8_t>& buffer, SessionListRequest& out);

std::vector<std::uint8_t> SerializeSessionListResponse(const SessionListResponse& resp);
bool ParseSessionListResponse(const std::vector<std::uint8_t>& buffer, SessionListResponse& out);

std::vector<std::uint8_t> SerializeStatsReport(const StatsReport& rpt);
bool ParseStatsReport(const std::vector<std::uint8_t>& buffer, StatsReport& out);

std::vector<std::uint8_t> SerializeStatsHistoryRequest(const StatsHistoryRequest& req);
bool ParseStatsHistoryRequest(const std::vector<std::uint8_t>& buffer, StatsHistoryRequest& out);

std::vector<std::uint8_t> SerializeStatsHistoryResponse(const StatsHistoryResponse& resp);
bool ParseStatsHistoryResponse(const std::vector<std::uint8_t>& buffer, StatsHistoryResponse& out);
}  // namespace mi::shared::proto
