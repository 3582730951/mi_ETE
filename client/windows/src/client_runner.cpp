#include "client/client_runner.hpp"

#include <algorithm>
#include <chrono>
#include <codecvt>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "client/secure_types.hpp"
#include "mi/shared/crypto/whitebox_aes.hpp"
#include "mi/shared/crypto/cert_store.hpp"
#include "mi/shared/crypto/tls_support.hpp"
#include "mi/shared/net/kcp_channel.hpp"
#include "mi/shared/proto/messages.hpp"
#include "mi/shared/storage/disordered_file.hpp"
#include "mi/shared/storage/chat_history.hpp"

namespace
{
constexpr std::uint8_t kAuthRequestType = 0x01;
constexpr std::uint8_t kDataPacketType = 0x02;
constexpr std::uint8_t kAuthResponseType = 0x11;
constexpr std::uint8_t kDataForwardType = 0x12;
constexpr std::uint8_t kErrorType = 0x13;
constexpr std::uint8_t kMediaChunkType = 0x03;
constexpr std::uint8_t kMediaForwardType = 0x23;
constexpr std::uint8_t kMediaControlType = 0x04;
constexpr std::uint8_t kMediaControlForwardType = 0x24;
constexpr std::uint8_t kChatMessageType = 0x05;
constexpr std::uint8_t kChatForwardType = 0x25;
constexpr std::uint8_t kChatControlType = 0x06;
constexpr std::uint8_t kChatControlForwardType = 0x26;
constexpr std::uint8_t kSessionListRequestType = 0x07;
constexpr std::uint8_t kSessionListResponseType = 0x27;
constexpr std::uint8_t kTlsClientHelloType = 0x30;
constexpr std::uint8_t kTlsServerHelloType = 0x31;
constexpr std::uint8_t kSecureEnvelopeType = 0x32;
constexpr std::uint8_t kChatAckAction = 2;   // 送达回执
constexpr std::uint8_t kChatReadAction = 3;  // 已读回执
constexpr std::uint8_t kStatsReportType = 0x28;
constexpr std::uint8_t kStatsAckType = 0x08;

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

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& buffer, std::size_t offset = 0)
{
    if (buffer.size() < offset + 4)
    {
        return 0;
    }
    return static_cast<std::uint32_t>(buffer[offset]) | (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) | (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
}

void WriteLe32(std::vector<std::uint8_t>& buffer, std::uint32_t value)
{
    buffer.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    buffer.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    buffer.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    buffer.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::uint64_t GenerateMediaId()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 gen(static_cast<std::uint64_t>(now));
    std::uniform_int_distribution<std::uint64_t> dist;
    return dist(gen) ^ static_cast<std::uint64_t>(now);
}

std::vector<std::uint8_t> BuildDynamicKey(std::uint32_t sessionId)
{
    std::vector<std::uint8_t> dyn(4);
    dyn[0] = static_cast<std::uint8_t>((sessionId >> 24) & 0xFF);
    dyn[1] = static_cast<std::uint8_t>((sessionId >> 16) & 0xFF);
    dyn[2] = static_cast<std::uint8_t>((sessionId >> 8) & 0xFF);
    dyn[3] = static_cast<std::uint8_t>((sessionId)&0xFF);
    return dyn;
}

std::vector<std::uint8_t> TrimPayload(const std::vector<std::uint8_t>& data, std::size_t maxSize = 512u * 1024u)
{
    if (data.size() <= maxSize)
    {
        return data;
    }
    return std::vector<std::uint8_t>(data.begin(), data.begin() + static_cast<long long>(maxSize));
}

std::vector<std::uint8_t> GenerateRandomBytes(std::size_t len)
{
    std::vector<std::uint8_t> out(len);
    std::random_device rd;
    for (std::size_t i = 0; i < len; ++i)
    {
        out[i] = static_cast<std::uint8_t>(rd());
    }
    return out;
}

mi::shared::crypto::WhiteboxKeyInfo BuildTlsKey(const std::vector<std::uint8_t>& secret)
{
    mi::shared::crypto::WhiteboxKeyInfo info{};
    info.keyParts = secret;
    if (info.keyParts.empty())
    {
        info.keyParts.push_back(0x5Au);
    }
    return info;
}

struct MediaAssembler
{
    std::wstring name;
    std::uint32_t totalChunks = 0;
    std::uint32_t totalSize = 0;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::uint32_t received = 0;
    std::uint64_t receivedBytes = 0;
};

bool AddChunk(MediaAssembler& asmblr, const mi::shared::proto::MediaChunk& chunk)
{
    if (asmblr.chunks.empty())
    {
        asmblr.totalChunks = chunk.totalChunks;
        asmblr.totalSize = chunk.totalSize;
        asmblr.name = chunk.name;
        asmblr.chunks.resize(chunk.totalChunks);
    }

    if (chunk.chunkIndex >= asmblr.chunks.size())
    {
        return false;
    }
    if (asmblr.chunks[chunk.chunkIndex].empty())
    {
        asmblr.received++;
        asmblr.receivedBytes += static_cast<std::uint64_t>(chunk.payload.size());
    }
    asmblr.chunks[chunk.chunkIndex] = chunk.payload;
    return asmblr.received == asmblr.totalChunks;
}

void EmitLog(const mi::client::ClientCallbacks& callbacks,
             const std::wstring& msg,
             mi::client::ClientCallbacks::EventLevel level = mi::client::ClientCallbacks::EventLevel::Info,
             const std::wstring& category = L"general",
             mi::client::ClientCallbacks::Direction direction = mi::client::ClientCallbacks::Direction::None,
             std::uint64_t messageId = 0,
             const std::wstring& peer = L"",
             const std::vector<std::uint8_t>& payload = {},
             const std::vector<std::wstring>& attachments = {},
             std::uint8_t format = 0,
             std::uint8_t severity = 0,
             std::uint32_t retryAfterMs = 0)
{
    if (callbacks.onLog)
    {
        callbacks.onLog(msg);
    }
    if (callbacks.onEvent)
    {
        mi::client::ClientCallbacks::ClientEvent ev{};
        ev.level = level;
        ev.category = category;
        ev.message = msg;
        ev.direction = direction;
        ev.messageId = messageId;
        ev.peer = peer;
        ev.payload = payload;
        ev.attachments = attachments;
        ev.format = format;
        ev.severity = severity;
        ev.retryAfterMs = retryAfterMs;
        callbacks.onEvent(ev);
    }
    if ((category == L"chat" || category == L"media") && callbacks.onChatEvent)
    {
        mi::client::ClientCallbacks::ClientEvent ev{};
        ev.level = level;
        ev.category = category;
        ev.message = msg;
        ev.direction = direction;
        ev.messageId = messageId;
        ev.peer = peer;
        ev.payload = payload;
        ev.attachments = attachments;
        ev.format = format;
        callbacks.onChatEvent(ev);
    }
    if (!callbacks.onLog && !callbacks.onEvent)
    {
        std::wcout << msg << L"\n";
    }
}
}  // namespace

namespace mi::client
{
std::string Fingerprint(const std::vector<std::uint8_t>& data)
{
    std::uint64_t h1 = 1469598103934665603ULL;
    const std::uint64_t prime = 1099511628211ULL;
    for (auto b : data)
    {
        h1 ^= b;
        h1 *= prime;
    }
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 7; i >= 0; --i)
    {
        const std::uint8_t part = static_cast<std::uint8_t>((h1 >> (i * 8)) & 0xFF);
        if (part < 16)
        {
            oss << '0';
        }
        oss << static_cast<int>(part);
    }
    return oss.str();
}

static bool RunClientSingle(const ClientOptions& options,
                            const mi::shared::crypto::WhiteboxKeyInfo& baseKeyInfo,
                            const ClientCallbacks& callbacks)
{
    // 预留多次会话尝试：若上层未退出且 reconnectAttempts >0 可重入
    const std::filesystem::path mediaCache = std::filesystem::path(L"media_cache");
    const std::filesystem::path chatCache = std::filesystem::path(L"chat_cache");
    std::filesystem::create_directories(mediaCache);
    std::filesystem::create_directories(chatCache);

    const auto startTs = std::chrono::steady_clock::now();
    auto lastActive = startTs;
    mi::client::SecureInt32 counter(42);
    mi::client::SecureString message(L"hello kcp/aes");

    mi::shared::crypto::WhiteboxKeyInfo keyInfo = baseKeyInfo;
    if (keyInfo.keyParts.empty())
    {
        keyInfo.keyParts = {0x10u, 0x21u, 0x32u};
    }
    mi::shared::storage::DisorderedFileStore disStore(mediaCache, keyInfo.keyParts);
    mi::shared::storage::ChatHistoryStore chatStore(chatCache, keyInfo.keyParts);
    std::unordered_map<std::uint64_t, MediaAssembler> mediaAssemblers;
    std::unordered_map<std::uint64_t, mi::shared::storage::StoredFile> savedMedia;
    std::unordered_map<std::uint64_t, std::uint64_t> sentChatRecords;
    std::unordered_map<std::uint64_t, std::uint64_t> receivedChatRecords;
    std::vector<std::uint8_t> certMem = options.certBytes;
    if (certMem.empty())
    {
        certMem = mi::shared::crypto::LoadCertFromEnv();
    }
    bool allowSelfSigned = options.certAllowSelfSigned;
    if (const char* allowEnv = std::getenv("MI_CERT_ALLOW_SELF_SIGNED"))
    {
        const std::string val = allowEnv;
        allowSelfSigned = !(val == "0" || val == "false" || val == "FALSE");
    }
    std::string certPassword = options.certPassword;
    if (certPassword.empty())
    {
        const char* envPwd = std::getenv("MI_CERT_PWD");
        if (envPwd != nullptr)
        {
            certPassword = envPwd;
        }
    }
    std::string expectedFingerprint = options.certFingerprint;
    if (expectedFingerprint.empty())
    {
        const char* env = std::getenv("MI_CERT_FPR");
        if (env != nullptr)
        {
            expectedFingerprint = env;
        }
    }
    mi::shared::crypto::WhiteboxKeyInfo transportKey{};
    bool tlsReady = false;
    std::vector<std::uint8_t> tlsSecret;

    std::vector<std::uint8_t> plainPayload;
    if (!options.message.empty())
    {
        const std::string utf8 = WideToUtf8(options.message);
        plainPayload.assign(utf8.begin(), utf8.end());
    }
    else
    {
        plainPayload = {'s', 'e', 'c', 'u', 'r', 'e', '_', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    }
    EmitLog(callbacks, L"[client] 发送模式: " + std::to_wstring(static_cast<int>(options.sendMode)), mi::client::ClientCallbacks::EventLevel::Info, L"startup");

    std::vector<std::uint8_t> mediaBytes;
    std::wstring mediaName;
    if (!options.mediaPath.empty())
    {
        mediaBytes = ReadFileBytes(options.mediaPath);
        mediaName = std::filesystem::path(options.mediaPath).filename().wstring();
        if (mediaBytes.empty())
        {
            EmitLog(callbacks, L"[client] 媒体文件读取失败: " + options.mediaPath,
                    mi::client::ClientCallbacks::EventLevel::Error, L"media");
        }
        else
        {
            EmitLog(callbacks,
                    L"[client] 读取媒体文件 " + mediaName + L" 大小=" + std::to_wstring(mediaBytes.size()),
                    mi::client::ClientCallbacks::EventLevel::Info, L"media");
        }
    }

    const auto sessionDyn = BuildDynamicKey(static_cast<std::uint32_t>(
        (std::chrono::high_resolution_clock::now().time_since_epoch().count()) & 0xFFFFFFFFu));
    const auto sessionKey = mi::shared::crypto::MixKey(keyInfo, sessionDyn);
    const auto cipher = mi::shared::crypto::Encrypt(plainPayload, sessionKey);
    const auto restored = mi::shared::crypto::Decrypt(cipher, sessionKey);
    const bool payloadOk = (plainPayload == restored);

    try
    {
        if (!plainPayload.empty())
        {
            const auto storedText = disStore.Save(L"out_text.bin", plainPayload, sessionDyn);
            EmitLog(callbacks, L"[client] 已乱序落盘文本 " + storedText.path.wstring(),
                    mi::client::ClientCallbacks::EventLevel::Info, L"storage");
        }
        if (!mediaBytes.empty())
        {
            const auto storedMedia = disStore.Save(mediaName.empty() ? L"media.bin" : mediaName, mediaBytes, sessionDyn);
            EmitLog(callbacks, L"[client] 已乱序落盘待发送媒体 " + storedMedia.path.wstring(),
                    mi::client::ClientCallbacks::EventLevel::Info, L"storage");
        }
    }
    catch (const std::exception& ex)
    {
        EmitLog(callbacks, L"[client] 乱序落盘失败: " + Utf8ToWide(ex.what()),
                mi::client::ClientCallbacks::EventLevel::Error, L"storage");
    }

    mi::shared::proto::AuthRequest req{};
    req.username = options.username;
    req.password = options.password;

    std::vector<std::uint8_t> authBuf;
    authBuf.push_back(kAuthRequestType);
    const auto authBody = mi::shared::proto::SerializeAuthRequest(req);
    authBuf.insert(authBuf.end(), authBody.begin(), authBody.end());

    mi::shared::net::KcpChannel channel;
    mi::shared::net::KcpSettings settings{};
    channel.Configure(settings);
    if (!channel.Start(L"0.0.0.0", 0))
    {
        EmitLog(callbacks, L"[client] KCP 绑定失败", mi::client::ClientCallbacks::EventLevel::Error, L"kcp");
        if (callbacks.onFinished)
        {
            callbacks.onFinished(false);
        }
        return false;
    }
    EmitLog(callbacks, L"[client] KCP 已绑定本地端口 " + std::to_wstring(channel.BoundPort()),
            mi::client::ClientCallbacks::EventLevel::Info, L"kcp");

    mi::shared::net::PeerEndpoint serverPeer{options.serverHost, options.serverPort};
    std::uint32_t sessionId = 0;
    for (std::uint32_t attempt = 0; attempt <= options.retryCount && sessionId == 0; ++attempt)
    {
        channel.Send(serverPeer, authBuf);
        const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
        while (std::chrono::steady_clock::now() < waitDeadline && sessionId == 0)
        {
            if (callbacks.isCancelled && callbacks.isCancelled())
            {
                channel.Stop();
                if (callbacks.onFinished)
                {
                    callbacks.onFinished(false);
                }
                return false;
            }
            channel.Poll();
            mi::shared::net::ReceivedDatagram packet{};
            while (channel.TryReceive(packet))
            {
                if (packet.payload.empty())
                {
                    continue;
                }
                const std::uint8_t type = packet.payload[0];
                const std::vector<std::uint8_t> body(packet.payload.begin() + 1, packet.payload.end());
                if (type == kAuthResponseType)
                {
                    mi::shared::proto::AuthResponse resp{};
                    if (mi::shared::proto::ParseAuthResponse(body, resp) && resp.success)
                    {
                        sessionId = resp.sessionId;
                        EmitLog(callbacks, L"[client] 认证成功 session=" + std::to_wstring(sessionId),
                                mi::client::ClientCallbacks::EventLevel::Success, L"auth",
                                mi::client::ClientCallbacks::Direction::Inbound, 0, std::to_wstring(sessionId));
                    }
                }
                else if (type == kErrorType)
                {
                    mi::shared::proto::ErrorResponse err{};
                    if (mi::shared::proto::ParseErrorResponse(body, err))
                    {
                        EmitLog(callbacks,
                                L"[client] 收到错误 code=" + std::to_wstring(static_cast<int>(err.code)) + L" msg=" +
                                    err.message,
                                mi::client::ClientCallbacks::EventLevel::Error, L"auth");
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (sessionId == 0 && attempt < options.retryCount)
        {
            EmitLog(callbacks, L"[client] 认证超时，重试 " + std::to_wstring(attempt + 1),
                    mi::client::ClientCallbacks::EventLevel::Error, L"auth");
            std::this_thread::sleep_for(std::chrono::milliseconds(options.retryDelayMs));
        }
    }

    if (sessionId == 0)
    {
        EmitLog(callbacks, L"[client] 未获得 session，退出", mi::client::ClientCallbacks::EventLevel::Error, L"auth");
        channel.Stop();
        if (callbacks.onFinished)
        {
        callbacks.onFinished(false);
        }
        return false;
    }

    auto unwrapEnvelope = [&](std::uint8_t& msgType, std::vector<std::uint8_t>& msgPayload) -> bool {
        if (msgType != kSecureEnvelopeType)
        {
            return true;
        }
        if (!tlsReady || transportKey.keyParts.empty())
        {
            return false;
        }
        const auto plain = mi::shared::crypto::Decrypt(msgPayload, transportKey);
        if (plain.empty())
        {
            return false;
        }
        msgType = plain[0];
        msgPayload.assign(plain.begin() + 1, plain.end());
        return true;
    };

    if (!certMem.empty())
    {
        const std::wstring pwdW(certPassword.begin(), certPassword.end());
        const auto chain = mi::shared::crypto::ValidatePfxChain(certMem, pwdW, allowSelfSigned);
        if (!chain.ok)
        {
            EmitLog(callbacks,
                    L"[client] 证书链校验失败: " + chain.error,
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"cert",
                    mi::client::ClientCallbacks::Direction::None,
                    sessionId);
            channel.Stop();
            if (callbacks.onFinished)
            {
                callbacks.onFinished(false);
            }
            return false;
        }
        if (!expectedFingerprint.empty() && !chain.fingerprintHex.empty() && chain.fingerprintHex != expectedFingerprint)
        {
            EmitLog(callbacks,
                    L"[client] 证书指纹不匹配，期望 " + Utf8ToWide(expectedFingerprint) + L" 实际 " +
                        Utf8ToWide(chain.fingerprintHex),
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"cert",
                    mi::client::ClientCallbacks::Direction::None,
                    sessionId);
            channel.Stop();
            if (callbacks.onFinished)
            {
                callbacks.onFinished(false);
            }
            return false;
        }

        tlsSecret = GenerateRandomBytes(32);
        std::vector<std::uint8_t> enc;
        if (!mi::shared::crypto::EncryptWithCertificate(certMem, pwdW, tlsSecret, enc))
        {
            EmitLog(callbacks,
                    L"[client] TLS 握手加密失败，无法使用证书",
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"cert",
                    mi::client::ClientCallbacks::Direction::None,
                    sessionId);
            channel.Stop();
            if (callbacks.onFinished)
            {
                callbacks.onFinished(false);
            }
            return false;
        }

        std::vector<std::uint8_t> hello;
        hello.push_back(kTlsClientHelloType);
        WriteLe32(hello, sessionId);
        hello.insert(hello.end(), enc.begin(), enc.end());
        channel.Send(serverPeer, hello, sessionId);

        const auto expectedHash = mi::shared::crypto::Sha256(tlsSecret);
        const auto hsDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
        bool handshakeOk = false;
        while (std::chrono::steady_clock::now() < hsDeadline && !handshakeOk)
        {
            channel.Poll();
            mi::shared::net::ReceivedDatagram packet{};
            while (channel.TryReceive(packet))
            {
                if (packet.payload.empty())
                {
                    continue;
                }
                std::uint8_t ptype = packet.payload[0];
                std::vector<std::uint8_t> pbody(packet.payload.begin() + 1, packet.payload.end());
                unwrapEnvelope(ptype, pbody);
                if (ptype == kTlsServerHelloType)
                {
                    if (expectedHash.empty() || pbody.size() < 4 + expectedHash.size())
                    {
                        continue;
                    }
                    const std::uint32_t respSid = ReadLe32(pbody, 0);
                    if (respSid != sessionId)
                    {
                        continue;
                    }
                    const std::vector<std::uint8_t> hashResp(pbody.begin() + 4,
                                                             pbody.begin() + 4 + expectedHash.size());
                    if (hashResp == expectedHash)
                    {
                        transportKey = BuildTlsKey(tlsSecret);
                        tlsReady = true;
                        handshakeOk = true;
                        break;
                    }
                }
            }
            if (!handshakeOk)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (!handshakeOk)
        {
            EmitLog(callbacks,
                    L"[client] TLS 握手未完成，连接中断",
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"cert",
                    mi::client::ClientCallbacks::Direction::None,
                    sessionId);
            channel.Stop();
            if (callbacks.onFinished)
            {
                callbacks.onFinished(false);
            }
            return false;
        }
        else
        {
            EmitLog(callbacks,
                    L"[client] TLS 握手完成，链路已加密 指纹=" + Utf8ToWide(chain.fingerprintHex),
                    mi::client::ClientCallbacks::EventLevel::Success,
                    L"cert",
                    mi::client::ClientCallbacks::Direction::None,
                    sessionId);
        }
    }

    auto sendFrame = [&](const std::vector<std::uint8_t>& plain) -> std::size_t {
        if (tlsReady && !transportKey.keyParts.empty())
        {
            const auto cipher = mi::shared::crypto::Encrypt(plain, transportKey);
            std::vector<std::uint8_t> env;
            env.push_back(kSecureEnvelopeType);
            env.insert(env.end(), cipher.begin(), cipher.end());
            channel.Send(serverPeer, env, sessionId);
            return env.size();
        }
        channel.Send(serverPeer, plain, sessionId);
        return plain.size();
    };

    if (options.subscribeSessions)
    {
        mi::shared::proto::SessionListRequest req{};
        req.sessionId = sessionId;
        req.subscribe = true;
        std::vector<std::uint8_t> reqBuf;
        reqBuf.push_back(kSessionListRequestType);
        const auto reqBody = mi::shared::proto::SerializeSessionListRequest(req);
        reqBuf.insert(reqBuf.end(), reqBody.begin(), reqBody.end());
        sendFrame(reqBuf);
        EmitLog(callbacks,
                L"[client] 请求会话列表订阅",
                mi::client::ClientCallbacks::EventLevel::Info,
                L"session",
                mi::client::ClientCallbacks::Direction::Outbound);
    }

    const bool sendChat = options.sendMode != SendMode::Data;
    const bool sendData = options.sendMode != SendMode::Chat;
    const std::uint32_t targetSession =
        (options.targetSessionId != 0) ? options.targetSessionId : sessionId;  // 回显
    std::uint64_t messageId = 0;
    std::uint32_t dataResendCount = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t sentMediaId = 0;

    if (sendChat)
    {
        messageId = GenerateMediaId();
        mi::shared::proto::ChatMessage chatMsg{};
        std::vector<std::wstring> attachmentsForLog;
        if (!options.mediaPath.empty())
        {
            const std::wstring mediaCombined = options.mediaPath;
            std::wstringstream ss(mediaCombined);
            std::wstring item;
            while (std::getline(ss, item, L';'))
            {
                if (!item.empty())
                {
                    const std::wstring name = std::filesystem::path(item).filename().wstring();
                    chatMsg.attachments.push_back(name);
                    attachmentsForLog.push_back(name);
                }
            }
        }
        chatMsg.sessionId = sessionId;
        chatMsg.targetSessionId = targetSession;
        chatMsg.messageId = messageId;
        chatMsg.payload = cipher;
        chatMsg.format = 1;
        std::vector<std::uint8_t> chatBuf;
        chatBuf.push_back(kChatMessageType);
        const auto chatBody = mi::shared::proto::SerializeChatMessage(chatMsg);
        chatBuf.insert(chatBuf.end(), chatBody.begin(), chatBody.end());
        const auto sentLen = sendFrame(chatBuf);
        bytesSent += sentLen;
        EmitLog(callbacks,
                L"[client] 发送聊天 messageId=" + std::to_wstring(messageId) + L" 长度=" +
                    std::to_wstring(plainPayload.size()),
                mi::client::ClientCallbacks::EventLevel::Info,
                L"chat",
                mi::client::ClientCallbacks::Direction::Outbound,
                messageId,
                std::to_wstring(targetSession),
                {},
                attachmentsForLog,
                chatMsg.format);
        try
        {
            mi::shared::storage::ChatOptions opts{};
            opts.dynamicKey = sessionDyn;
            opts.name = L"chat_out.msg";
            opts.format = chatMsg.format;
            opts.attachments = chatMsg.attachments;
            const auto rec = chatStore.Append(sessionId, options.serverHost, plainPayload, opts);
            sentChatRecords[messageId] = rec.id;
        }
        catch (const std::exception& ex)
        {
            EmitLog(callbacks, L"[client] 记录聊天历史失败: " + Utf8ToWide(ex.what()),
                    mi::client::ClientCallbacks::EventLevel::Error, L"chat");
        }
    }

    if (sendData)
    {
        mi::shared::proto::DataPacket data{};
        data.sessionId = sessionId;
        data.targetSessionId = targetSession;
        data.payload = cipher;
        std::vector<std::uint8_t> dataBuf;
        dataBuf.push_back(kDataPacketType);
        const auto dataBody = mi::shared::proto::SerializeDataPacket(data);
        dataBuf.insert(dataBuf.end(), dataBody.begin(), dataBody.end());
        const auto dataLen = sendFrame(dataBuf);
        bytesSent += dataLen;
    }

    if (!mediaBytes.empty())
    {
        const std::uint32_t chunkSize = std::max<std::uint32_t>(256, options.mediaChunkSize);
        const std::uint32_t totalChunks =
            static_cast<std::uint32_t>((mediaBytes.size() + chunkSize - 1u) / chunkSize);
        sentMediaId = GenerateMediaId();
        EmitLog(callbacks,
                L"[client] 发送媒体 id=" + std::to_wstring(sentMediaId) + L" 大小=" +
                    std::to_wstring(mediaBytes.size()),
                mi::client::ClientCallbacks::EventLevel::Info,
                L"media",
                mi::client::ClientCallbacks::Direction::Outbound,
                sentMediaId,
                std::to_wstring(targetSession),
                TrimPayload(mediaBytes));
        for (std::uint32_t i = 0; i < totalChunks; ++i)
        {
            const std::size_t offset = static_cast<std::size_t>(i) * static_cast<std::size_t>(chunkSize);
            const std::size_t sz = std::min<std::size_t>(chunkSize, mediaBytes.size() - offset);
            std::vector<std::uint8_t> chunk(mediaBytes.begin() + static_cast<long long>(offset),
                                            mediaBytes.begin() + static_cast<long long>(offset + sz));
            const auto cipherChunk = mi::shared::crypto::Encrypt(chunk, sessionKey);

            mi::shared::proto::MediaChunk mediaPkt{};
            mediaPkt.sessionId = sessionId;
            mediaPkt.targetSessionId = targetSession;
            mediaPkt.mediaId = sentMediaId;
            mediaPkt.chunkIndex = i;
            mediaPkt.totalChunks = totalChunks;
            mediaPkt.totalSize = static_cast<std::uint32_t>(mediaBytes.size());
            mediaPkt.name = mediaName.empty() ? L"media.bin" : mediaName;
            mediaPkt.payload = cipherChunk;

            std::vector<std::uint8_t> mediaBuf;
            mediaBuf.push_back(kMediaChunkType);
            const auto mediaBody = mi::shared::proto::SerializeMediaChunk(mediaPkt);
            mediaBuf.insert(mediaBuf.end(), mediaBody.begin(), mediaBody.end());
            const auto mlen = sendFrame(mediaBuf);
            bytesSent += mlen;

            if (callbacks.onProgress)
            {
                mi::client::ClientCallbacks::ProgressEvent prog{};
                prog.value = static_cast<double>(i + 1) / static_cast<double>(totalChunks);
                prog.mediaId = sentMediaId;
                prog.direction = mi::client::ClientCallbacks::Direction::Outbound;
                prog.chunkIndex = i + 1;
                prog.totalChunks = totalChunks;
                prog.bytesTransferred = static_cast<std::uint32_t>(std::min<std::size_t>(mediaBytes.size(), offset + sz));
                prog.totalBytes = static_cast<std::uint32_t>(mediaBytes.size());
                callbacks.onProgress(prog);
            }
        }
        EmitLog(callbacks, L"[client] 媒体发送完成 id=" + std::to_wstring(sentMediaId),
                mi::client::ClientCallbacks::EventLevel::Info, L"media");
    }

    const auto loopStart = std::chrono::steady_clock::now();
    const std::uint32_t maxAttempts = std::max<std::uint32_t>(1u, options.retryCount + 1u);
    const std::uint32_t totalWindowMs =
        options.timeoutMs + static_cast<std::uint32_t>(options.retryDelayMs * (maxAttempts > 0 ? (maxAttempts - 1u) : 0u));
    const auto dataDeadline = loopStart + std::chrono::milliseconds(totalWindowMs);
    bool receivedChatEcho = !sendChat;
    bool chatAcked = !sendChat;
    bool receivedDataEcho = !sendData;
    bool mediaReceived = mediaBytes.empty();
    bool chatControlAck = !sendChat;
    std::uint32_t chatAttempts = sendChat ? 1u : 0u;
    std::uint32_t dataAttempts = sendData ? 1u : 0u;
    std::uint32_t mediaAttempts = mediaBytes.empty() ? 0u : 1u;
    std::uint32_t failedChatCount = 0;
    std::uint32_t failedDataCount = 0;
    std::uint32_t failedMediaCount = 0;
    auto nextSessionListPoll = loopStart + std::chrono::seconds(4);
    auto nextChatSend = loopStart + std::chrono::milliseconds(options.retryDelayMs);
    auto nextDataSend = loopStart + std::chrono::milliseconds(options.retryDelayMs);
    auto nextMediaSend = loopStart + std::chrono::milliseconds(options.retryDelayMs);
    while (std::chrono::steady_clock::now() < dataDeadline && (!chatAcked || !receivedDataEcho || !mediaReceived))
    {
        if (callbacks.isCancelled && callbacks.isCancelled())
        {
            EmitLog(callbacks, L"[client] 外部取消", mi::client::ClientCallbacks::EventLevel::Error, L"control");
            channel.Stop();
            if (callbacks.onFinished)
            {
                callbacks.onFinished(false);
            }
            return false;
        }
        channel.Poll();
        lastActive = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (options.subscribeSessions && now >= nextSessionListPoll)
        {
            mi::shared::proto::SessionListRequest ping{};
            ping.sessionId = sessionId;
            ping.subscribe = true;
            std::vector<std::uint8_t> buf;
            buf.push_back(kSessionListRequestType);
            const auto body = mi::shared::proto::SerializeSessionListRequest(ping);
            buf.insert(buf.end(), body.begin(), body.end());
            sendFrame(buf);
            nextSessionListPoll = now + std::chrono::seconds(4);
        }
        if (sendChat && !chatAcked && chatAttempts < maxAttempts && now >= nextChatSend)
        {
            mi::shared::proto::ChatMessage chatMsg{};
            chatMsg.sessionId = sessionId;
            chatMsg.targetSessionId = targetSession;
            chatMsg.messageId = messageId;
            chatMsg.payload = cipher;
            chatMsg.format = 1;  // markdown/html for UI richness
            if (!options.mediaPath.empty())
            {
                chatMsg.attachments.push_back(std::filesystem::path(options.mediaPath).filename().wstring());
            }
            std::vector<std::uint8_t> chatBuf;
            chatBuf.push_back(kChatMessageType);
            const auto chatBody = mi::shared::proto::SerializeChatMessage(chatMsg);
            chatBuf.insert(chatBuf.end(), chatBody.begin(), chatBody.end());
            const auto clen = sendFrame(chatBuf);
            bytesSent += clen;
            dataResendCount++;
            chatAttempts++;
            nextChatSend = now + std::chrono::milliseconds(options.retryDelayMs);
            EmitLog(callbacks,
                    L"[client] 重发聊天 messageId=" + std::to_wstring(messageId) + L" 第 " +
                        std::to_wstring(chatAttempts) + L"/" + std::to_wstring(maxAttempts),
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"retry");
        }
        if (sendData && !receivedDataEcho && dataAttempts < maxAttempts && now >= nextDataSend)
        {
            mi::shared::proto::DataPacket data{};
            data.sessionId = sessionId;
            data.targetSessionId = targetSession;
            data.payload = cipher;
            std::vector<std::uint8_t> dataBuf;
            dataBuf.push_back(kDataPacketType);
            const auto dataBody = mi::shared::proto::SerializeDataPacket(data);
            dataBuf.insert(dataBuf.end(), dataBody.begin(), dataBody.end());
            const auto dlen = sendFrame(dataBuf);
            bytesSent += dlen;
            dataAttempts++;
            dataResendCount++;
            nextDataSend = now + std::chrono::milliseconds(options.retryDelayMs);
            EmitLog(callbacks,
                    L"[client] 重发数据包 第 " + std::to_wstring(dataAttempts) + L"/" + std::to_wstring(maxAttempts),
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"retry");
        }
        if (!mediaBytes.empty() && !mediaReceived && mediaAttempts < maxAttempts && now >= nextMediaSend && sentMediaId != 0)
        {
            const std::uint32_t chunkSize = std::max<std::uint32_t>(256, options.mediaChunkSize);
            const std::uint32_t totalChunks =
                static_cast<std::uint32_t>((mediaBytes.size() + chunkSize - 1u) / chunkSize);
            for (std::uint32_t i = 0; i < totalChunks; ++i)
            {
                const std::size_t offset = static_cast<std::size_t>(i) * static_cast<std::size_t>(chunkSize);
                const std::size_t sz = std::min<std::size_t>(chunkSize, mediaBytes.size() - offset);
                std::vector<std::uint8_t> chunk(mediaBytes.begin() + static_cast<long long>(offset),
                                                mediaBytes.begin() + static_cast<long long>(offset + sz));
                const auto cipherChunk = mi::shared::crypto::Encrypt(chunk, sessionKey);

                mi::shared::proto::MediaChunk mediaPkt{};
                mediaPkt.sessionId = sessionId;
                mediaPkt.targetSessionId = targetSession;
                mediaPkt.mediaId = sentMediaId;
                mediaPkt.chunkIndex = i;
                mediaPkt.totalChunks = totalChunks;
                mediaPkt.totalSize = static_cast<std::uint32_t>(mediaBytes.size());
                mediaPkt.name = mediaName.empty() ? L"media.bin" : mediaName;
                mediaPkt.payload = cipherChunk;

                std::vector<std::uint8_t> mediaBuf;
                mediaBuf.push_back(kMediaChunkType);
                const auto mediaBody = mi::shared::proto::SerializeMediaChunk(mediaPkt);
                mediaBuf.insert(mediaBuf.end(), mediaBody.begin(), mediaBody.end());
                const auto resent = sendFrame(mediaBuf);
                bytesSent += resent;
            }
            mediaAttempts++;
            nextMediaSend = now + std::chrono::milliseconds(options.retryDelayMs);
            EmitLog(callbacks,
                    L"[client] 重发媒体 id=" + std::to_wstring(sentMediaId) + L" 第 " +
                        std::to_wstring(mediaAttempts) + L"/" + std::to_wstring(maxAttempts),
                    mi::client::ClientCallbacks::EventLevel::Error,
                    L"retry");
        }
        mi::shared::net::ReceivedDatagram packet{};
        while (channel.TryReceive(packet))
        {
            lastActive = std::chrono::steady_clock::now();
            if (packet.payload.empty())
            {
                continue;
            }
            std::uint8_t type = packet.payload[0];
            std::vector<std::uint8_t> body(packet.payload.begin() + 1, packet.payload.end());
            if (!unwrapEnvelope(type, body))
            {
                continue;
            }
            if (type == kDataForwardType)
            {
                mi::shared::proto::DataPacket parsed{};
                if (mi::shared::proto::ParseDataPacket(body, parsed))
                {
                    const auto decrypted = mi::shared::crypto::Decrypt(parsed.payload, sessionKey);
                    EmitLog(callbacks,
                            L"[client] 收到回显 session=" + std::to_wstring(parsed.sessionId) +
                                L" 文本大小=" + std::to_wstring(decrypted.size()) + L" (data)",
                            mi::client::ClientCallbacks::EventLevel::Info,
                            L"data",
                            mi::client::ClientCallbacks::Direction::Inbound,
                            0,
                            L"",
                            decrypted);
                    receivedDataEcho = (decrypted == plainPayload);
                    bytesReceived += decrypted.size();
        if (!decrypted.empty())
        {
            try
            {
                chatStore.Append(sessionId, L"peer", decrypted, {sessionDyn, L"data_in.msg"});
            }
            catch (const std::exception& ex)
            {
                EmitLog(callbacks, L"[client] 记录入站聊天失败: " + Utf8ToWide(ex.what()),
                        mi::client::ClientCallbacks::EventLevel::Error, L"chat");
            }
        }
                }
            }
            else if (type == kChatForwardType)
            {
                mi::shared::proto::ChatMessage chatPkt{};
                if (!mi::shared::proto::ParseChatMessage(body, chatPkt))
                {
                    continue;
                }
                const auto decrypted = mi::shared::crypto::Decrypt(chatPkt.payload, sessionKey);
                receivedChatEcho = (chatPkt.messageId == messageId && decrypted == plainPayload) || receivedChatEcho;
                chatAcked = chatAcked || (chatPkt.messageId == messageId);
                bytesReceived += decrypted.size();
                if (!decrypted.empty())
                {
                    try
                    {
                        mi::shared::storage::ChatOptions opts{};
                        opts.dynamicKey = sessionDyn;
                        opts.name = L"chat_in.msg";
                        opts.format = chatPkt.format;
                        opts.attachments = chatPkt.attachments;
                        const auto rec = chatStore.Append(sessionId, L"peer", decrypted, opts);
                        receivedChatRecords[chatPkt.messageId] = rec.id;
                    }
                    catch (const std::exception& ex)
                    {
                        EmitLog(callbacks, L"[client] 记录聊天失败: " + Utf8ToWide(ex.what()),
                                mi::client::ClientCallbacks::EventLevel::Error, L"chat");
                    }
                }
                EmitLog(callbacks,
                        L"[client] 收到聊天 messageId=" + std::to_wstring(chatPkt.messageId) + L" 来自 " +
                            std::to_wstring(chatPkt.sessionId),
                        mi::client::ClientCallbacks::EventLevel::Info,
                        L"chat",
                        mi::client::ClientCallbacks::Direction::Inbound,
                        chatPkt.messageId,
                        std::to_wstring(chatPkt.sessionId),
                        decrypted,
                        chatPkt.attachments,
                        chatPkt.format);
                // 发送协议级送达/已读回执
                mi::shared::proto::ChatControl ack{};
                ack.sessionId = sessionId;
                ack.targetSessionId = chatPkt.sessionId;
                ack.messageId = chatPkt.messageId;
                ack.action = kChatAckAction;
                std::vector<std::uint8_t> ackBuf;
                ackBuf.push_back(kChatControlType);
                const auto ackBody = mi::shared::proto::SerializeChatControl(ack);
                ackBuf.insert(ackBuf.end(), ackBody.begin(), ackBody.end());
                sendFrame(ackBuf);
                EmitLog(callbacks,
                        L"[client] 发送送达回执 messageId=" + std::to_wstring(chatPkt.messageId),
                        mi::client::ClientCallbacks::EventLevel::Success,
                        L"chat",
                        mi::client::ClientCallbacks::Direction::Outbound,
                        chatPkt.messageId,
                        std::to_wstring(chatPkt.sessionId));
                mi::shared::proto::ChatControl readCtl{};
                readCtl.sessionId = sessionId;
                readCtl.targetSessionId = chatPkt.sessionId;
                readCtl.messageId = chatPkt.messageId;
                readCtl.action = kChatReadAction;
                std::vector<std::uint8_t> readBuf;
                readBuf.push_back(kChatControlType);
                const auto readBody = mi::shared::proto::SerializeChatControl(readCtl);
                readBuf.insert(readBuf.end(), readBody.begin(), readBody.end());
                sendFrame(readBuf);
                if (options.revokeAfterReceive && chatPkt.messageId == messageId && !chatControlAck && sendChat)
                {
                    mi::shared::proto::ChatControl ctl{};
                    ctl.sessionId = sessionId;
                    ctl.targetSessionId = chatPkt.targetSessionId;
                    ctl.messageId = chatPkt.messageId;
                    ctl.action = 1;
                    std::vector<std::uint8_t> ctlBuf;
                    ctlBuf.push_back(kChatControlType);
                    const auto ctlBody = mi::shared::proto::SerializeChatControl(ctl);
                    ctlBuf.insert(ctlBuf.end(), ctlBody.begin(), ctlBody.end());
                    sendFrame(ctlBuf);
                }
            }
            else if (type == kMediaForwardType)
            {
                mi::shared::proto::MediaChunk mediaPkt{};
                if (!mi::shared::proto::ParseMediaChunk(body, mediaPkt))
                {
                    continue;
                }
                MediaAssembler& asmblr = mediaAssemblers[mediaPkt.mediaId];
                const bool completed = AddChunk(asmblr, mediaPkt);
                if (callbacks.onProgress && asmblr.totalChunks > 0)
                {
                    mi::client::ClientCallbacks::ProgressEvent prog{};
                    prog.mediaId = mediaPkt.mediaId;
                    prog.direction = mi::client::ClientCallbacks::Direction::Inbound;
                    prog.chunkIndex = asmblr.received;
                    prog.totalChunks = asmblr.totalChunks;
                    prog.value = static_cast<double>(asmblr.received) / static_cast<double>(asmblr.totalChunks);
                    prog.bytesTransferred = std::min<std::uint64_t>(mediaPkt.totalSize, asmblr.receivedBytes);
                    prog.totalBytes = mediaPkt.totalSize;
                    callbacks.onProgress(prog);
                }
                if (completed)
                {
                    std::vector<std::uint8_t> assembledCipher;
                    assembledCipher.reserve(mediaPkt.totalSize);
                    for (const auto& ch : asmblr.chunks)
                    {
                        assembledCipher.insert(assembledCipher.end(), ch.begin(), ch.end());
                    }
                    const auto plain = mi::shared::crypto::Decrypt(assembledCipher, sessionKey);
                    bytesReceived += plain.size();
                    const auto dynKey = BuildDynamicKey(sessionId);
                    const auto stored = disStore.Save(mediaPkt.name, plain, dynKey);
                    savedMedia[mediaPkt.mediaId] = stored;
                    mediaAssemblers.erase(mediaPkt.mediaId);
                    mediaReceived = true;
                    EmitLog(callbacks,
                            L"[client] 媒体接收完成 id=" + std::to_wstring(mediaPkt.mediaId) + L" 保存为 " +
                                stored.path.wstring(),
                            mi::client::ClientCallbacks::EventLevel::Success,
                            L"media",
                            mi::client::ClientCallbacks::Direction::Inbound,
                            mediaPkt.mediaId,
                            std::to_wstring(mediaPkt.sessionId),
                            TrimPayload(plain));

                    if (options.revokeAfterReceive && sentMediaId == mediaPkt.mediaId)
                    {
                        mi::shared::proto::MediaControl ctl{};
                        ctl.sessionId = sessionId;
                        ctl.targetSessionId = targetSession;
                        ctl.mediaId = mediaPkt.mediaId;
                        ctl.action = 1;
                        std::vector<std::uint8_t> ctlBuf;
                        ctlBuf.push_back(kMediaControlType);
                        const auto ctlBody = mi::shared::proto::SerializeMediaControl(ctl);
                        ctlBuf.insert(ctlBuf.end(), ctlBody.begin(), ctlBody.end());
                        sendFrame(ctlBuf);
                        EmitLog(callbacks, L"[client] 已发送撤回指令 id=" + std::to_wstring(mediaPkt.mediaId),
                                mi::client::ClientCallbacks::EventLevel::Info, L"media");
                    }
                }
            }
            else if (type == kMediaControlForwardType)
            {
                mi::shared::proto::MediaControl ctl{};
                if (!mi::shared::proto::ParseMediaControl(body, ctl))
                {
                    continue;
                }
                auto it = savedMedia.find(ctl.mediaId);
                if (it != savedMedia.end())
                {
                    disStore.Revoke(it->second.id);
                    savedMedia.erase(it);
                    EmitLog(callbacks, L"[client] 已撤回媒体 id=" + std::to_wstring(ctl.mediaId),
                            mi::client::ClientCallbacks::EventLevel::Info, L"media",
                            mi::client::ClientCallbacks::Direction::Inbound, ctl.mediaId);
                }
            }
            else if (type == kChatControlForwardType)
            {
                mi::shared::proto::ChatControl ctl{};
                if (!mi::shared::proto::ParseChatControl(body, ctl))
                {
                    continue;
                }
                if (ctl.action == kChatAckAction)
                {
                    EmitLog(callbacks, L"[client] 收到协议回执 messageId=" + std::to_wstring(ctl.messageId),
                            mi::client::ClientCallbacks::EventLevel::Success,
                            L"chat",
                            mi::client::ClientCallbacks::Direction::Inbound,
                            ctl.messageId,
                            std::to_wstring(ctl.sessionId));
                    receivedChatEcho = receivedChatEcho || (ctl.messageId == messageId);
                    chatAcked = chatAcked || (ctl.messageId == messageId);
                }
                else if (ctl.action == kChatReadAction)
                {
                    EmitLog(callbacks, L"[client] 对端已读 messageId=" + std::to_wstring(ctl.messageId),
                            mi::client::ClientCallbacks::EventLevel::Success,
                            L"chat",
                            mi::client::ClientCallbacks::Direction::Inbound,
                            ctl.messageId,
                            std::to_wstring(ctl.sessionId));
                }
                else if (ctl.action == 1)
                {
                    auto it = receivedChatRecords.find(ctl.messageId);
                    if (it != receivedChatRecords.end())
                    {
                        chatStore.Revoke(it->second);
                        receivedChatRecords.erase(it);
                        EmitLog(callbacks, L"[client] 已撤回聊天 id=" + std::to_wstring(ctl.messageId),
                                mi::client::ClientCallbacks::EventLevel::Info, L"chat");
                    }
                    if (sentChatRecords.count(ctl.messageId))
                    {
                        chatStore.Revoke(sentChatRecords[ctl.messageId]);
                        chatControlAck = true;
                    }
                }
            }
            else if (type == kSessionListResponseType)
            {
                mi::shared::proto::SessionListResponse resp{};
                if (!mi::shared::proto::ParseSessionListResponse(body, resp))
                {
                    continue;
                }
                std::vector<std::pair<std::uint32_t, std::wstring>> sessions;
                sessions.reserve(resp.sessions.size());
                for (const auto& s : resp.sessions)
                {
                    sessions.emplace_back(s.sessionId, s.peer);
                }
                if (callbacks.onSessionList)
                {
                    callbacks.onSessionList(sessions);
                }
                EmitLog(callbacks,
                        L"[client] 收到会话列表 " + std::to_wstring(resp.sessions.size()) + L" 项",
                        mi::client::ClientCallbacks::EventLevel::Info,
                        L"session");
            }
            else if (type == kErrorType)
            {
                mi::shared::proto::ErrorResponse err{};
                if (mi::shared::proto::ParseErrorResponse(body, err))
                {
                    std::wstring msg = L"[client] 错误 code=" + std::to_wstring(static_cast<int>(err.code)) +
                                       L" sev=" + std::to_wstring(static_cast<int>(err.severity)) + L" msg=" + err.message;
                    if (err.retryAfterMs > 0)
                    {
                        msg += L" retryAfterMs=" + std::to_wstring(err.retryAfterMs);
                    }
                    EmitLog(callbacks,
                            msg,
                            mi::client::ClientCallbacks::EventLevel::Error,
                            L"error",
                            mi::client::ClientCallbacks::Direction::None,
                            0,
                            L"",
                            {},
                            {},
                            0,
                            err.severity,
                            err.retryAfterMs);
                    if (err.severity == 1)
                    {
                        const auto backoff = std::chrono::milliseconds(err.retryAfterMs > 0 ? err.retryAfterMs
                                                                                             : options.retryDelayMs);
                        nextChatSend = now + backoff;
                        nextDataSend = now + backoff;
                        nextMediaSend = now + backoff;
                    }
                }
            }
        }
        if (options.idleReconnectMs > 0)
        {
            const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lastActive)
                                    .count();
            if (idleMs > static_cast<long long>(options.idleReconnectMs))
            {
                EmitLog(callbacks,
                        L"[client] 长时间无流量，准备重连",
                        mi::client::ClientCallbacks::EventLevel::Error,
                        L"reconnect");
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (sendChat && !chatAcked)
    {
        EmitLog(callbacks,
                L"[client] 聊天未获得协议回执，已耗尽重试",
                mi::client::ClientCallbacks::EventLevel::Error,
                L"retry",
                mi::client::ClientCallbacks::Direction::Outbound,
                messageId,
                std::to_wstring(targetSession));
        failedChatCount = 1;
    }
    if (sendData && !receivedDataEcho)
    {
        EmitLog(callbacks,
                L"[client] 数据包未获得回显确认，重试结束",
                mi::client::ClientCallbacks::EventLevel::Error,
                L"retry",
                mi::client::ClientCallbacks::Direction::Outbound);
        failedDataCount = 1;
    }
    if (!mediaBytes.empty() && !mediaReceived)
    {
        EmitLog(callbacks,
                L"[client] 媒体未完成接收，重试结束",
                mi::client::ClientCallbacks::EventLevel::Error,
                L"media");
        failedMediaCount = 1;
    }

    const std::wstring stats = L"[client] 统计: sent=" + std::to_wstring(bytesSent) + L"B recv=" +
                               std::to_wstring(bytesReceived) + L"B chatAttempts=" +
                               std::to_wstring(chatAttempts) + L" dataAttempts=" + std::to_wstring(dataAttempts) +
                               L" mediaAttempts=" + std::to_wstring(mediaAttempts);
    EmitLog(callbacks, stats, mi::client::ClientCallbacks::EventLevel::Info, L"stats");

    const auto endTs = std::chrono::steady_clock::now();
    const double durationMs =
        static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(endTs - startTs).count());
    if (callbacks.onStats)
    {
        mi::client::ClientCallbacks::StatsEvent st{};
        st.bytesSent = bytesSent;
        st.bytesReceived = bytesReceived;
        st.chatAttempts = chatAttempts;
        st.dataAttempts = dataAttempts;
        st.mediaAttempts = mediaAttempts;
        st.chatFailures = failedChatCount;
        st.dataFailures = failedDataCount;
        st.mediaFailures = failedMediaCount;
        st.durationMs = durationMs;
        callbacks.onStats(st);
    }

    const bool success = (chatAcked && receivedDataEcho && mediaReceived);
    EmitLog(callbacks,
            L"mi_client 占位启动，计数=" + std::to_wstring(counter.Value()) + L"，文本=" + message.Value() +
                L"，模式=" + std::to_wstring(static_cast<int>(options.sendMode)) + L"，AES 回环=" +
                (payloadOk ? L"通过" : L"失败") + L"，回显校验=" +
                ((receivedChatEcho && receivedDataEcho) ? L"成功" : L"失败或超时") + L"，媒体状态=" +
                (mediaReceived ? L"完成" : L"未完成"),
            success ? mi::client::ClientCallbacks::EventLevel::Success
                    : mi::client::ClientCallbacks::EventLevel::Error);
    // 发送速率/失败统计给服务端
    mi::shared::proto::StatsReport report{};
    report.sessionId = sessionId;
    report.bytesSent = bytesSent;
    report.bytesReceived = bytesReceived;
    report.chatFailures = failedChatCount;
    report.dataFailures = failedDataCount;
    report.mediaFailures = failedMediaCount;
    report.durationMs = static_cast<std::uint32_t>(durationMs);
    {
        std::vector<std::uint8_t> certMem = options.certBytes;
        if (certMem.empty())
        {
            certMem = mi::shared::crypto::LoadCertFromEnv();
        }
        if (!certMem.empty())
        {
            const std::string fpr = Fingerprint(certMem);
            std::string expected = options.certFingerprint;
            if (expected.empty())
            {
                const char* env = std::getenv("MI_CERT_FPR");
                if (env != nullptr)
                {
                    expected = env;
                }
            }
            if (!expected.empty() && fpr != expected)
            {
                EmitLog(callbacks,
                        L"[client] 证书指纹不匹配，期望 " + Utf8ToWide(expected) + L" 实际 " + Utf8ToWide(fpr),
                        mi::client::ClientCallbacks::EventLevel::Error,
                        L"cert",
                        mi::client::ClientCallbacks::Direction::None,
                        0,
                        L"",
                        {},
                        {},
                        0,
                        2);
                return false;
            }
            EmitLog(callbacks,
                    L"[client] 已加载内存证书，长度=" + std::to_wstring(certMem.size()) + L" 指纹=" + Utf8ToWide(fpr) +
                        L"（不落地，仅内存校验占位）",
                    mi::client::ClientCallbacks::EventLevel::Info,
                    L"cert");
        }
    }
    std::vector<std::uint8_t> statsBuf;
    statsBuf.push_back(kStatsReportType);
    const auto statsBody = mi::shared::proto::SerializeStatsReport(report);
    statsBuf.insert(statsBuf.end(), statsBody.begin(), statsBody.end());
    sendFrame(statsBuf);

    channel.Stop();
    if (callbacks.onFinished)
    {
        callbacks.onFinished(success);
    }
    return success;
}

bool RunClient(const ClientOptions& options,
               const mi::shared::crypto::WhiteboxKeyInfo& baseKeyInfo,
               const ClientCallbacks& callbacks)
{
    for (std::uint32_t attempt = 0; attempt <= options.reconnectAttempts; ++attempt)
    {
        const bool ok = RunClientSingle(options, baseKeyInfo, callbacks);
        if (ok)
        {
            return true;
        }
        if (attempt >= options.reconnectAttempts)
        {
            break;
        }
        if (callbacks.onLog)
        {
            callbacks.onLog(L"[client] 失败，准备重连 attempt=" + std::to_wstring(attempt + 1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.reconnectDelayMs));
    }
    return false;
}
}  // namespace mi::client
