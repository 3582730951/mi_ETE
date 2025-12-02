#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mi/shared/crypto/whitebox_aes.hpp"

namespace mi::client
{
enum class SendMode
{
    Chat,
    Data,
    Both
};

struct ClientOptions
{
    std::wstring serverHost = L"127.0.0.1";
    uint16_t serverPort = 7845;
    std::wstring username = L"user";
    std::wstring password = L"pass";
    std::wstring message = L"secure_payload";
    std::uint32_t targetSessionId = 0;
    std::uint32_t timeoutMs = 2000;
    std::wstring mediaPath;
    std::uint32_t mediaChunkSize = 1200;
    bool revokeAfterReceive = false;
    std::uint32_t retryCount = 1;
    std::uint32_t retryDelayMs = 500;
    SendMode sendMode = SendMode::Chat;
    std::wstring configPath;
    bool subscribeSessions = true;
    std::uint32_t reconnectAttempts = 1;    // 失败后额外重连次数
    std::uint32_t reconnectDelayMs = 2000;  // 重连前等待
    std::uint32_t idleReconnectMs = 15000;  // 长时间无流量触发重连
    std::vector<std::uint8_t> certBytes;    // 证书内存块（不落地）
    std::string certFingerprint;            // 期望指纹（hex），可选
    std::string certPassword;               // 证书密码（PFX），可选
    bool certAllowSelfSigned = true;        // 是否允许自签
};

struct ClientCallbacks
{
    std::function<void(const std::wstring&)> onLog;
    enum class EventLevel
    {
        Info,
        Success,
        Error
    };
    enum class Direction
    {
        Inbound,
        Outbound,
        None
    };
    struct ClientEvent
    {
        EventLevel level = EventLevel::Info;
        std::wstring category;
        std::wstring message;
        Direction direction = Direction::None;
        std::uint64_t messageId = 0;
        std::wstring peer;
        std::vector<std::uint8_t> payload;
        std::vector<std::wstring> attachments;
        std::uint8_t format = 0;
        std::uint8_t severity = 0;
        std::uint32_t retryAfterMs = 0;
    };
    std::function<void(const ClientEvent&)> onChatEvent;  // 仅聊天/媒体事件
    std::function<void(const ClientEvent&)> onEvent;
    std::function<void(bool success)> onFinished;
    std::function<bool()> isCancelled;
    struct ProgressEvent
    {
        double value = 0.0;  // 0~1
        std::uint64_t mediaId = 0;
        Direction direction = Direction::None;
        std::uint32_t chunkIndex = 0;
        std::uint32_t totalChunks = 0;
        std::uint32_t bytesTransferred = 0;
        std::uint32_t totalBytes = 0;
    };
    std::function<void(const ProgressEvent&)> onProgress;  // 媒体发送/接收进度
    std::function<void(const std::vector<std::pair<std::uint32_t, std::wstring>>&)> onSessionList;
    struct StatsEvent
    {
        std::uint64_t bytesSent = 0;
        std::uint64_t bytesReceived = 0;
        std::uint32_t chatAttempts = 0;
        std::uint32_t dataAttempts = 0;
        std::uint32_t mediaAttempts = 0;
        std::uint32_t chatFailures = 0;
        std::uint32_t dataFailures = 0;
        std::uint32_t mediaFailures = 0;
        double durationMs = 0.0;
    };
    std::function<void(const StatsEvent&)> onStats;
};

bool RunClient(const ClientOptions& options,
               const mi::shared::crypto::WhiteboxKeyInfo& keyInfo,
               const ClientCallbacks& callbacks);
}  // namespace mi::client
