#include "server/message_router.hpp"

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>

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
constexpr std::uint8_t kChatMessageForwardType = 0x25;
constexpr std::uint8_t kChatControlType = 0x06;
constexpr std::uint8_t kChatControlForwardType = 0x26;
constexpr std::uint8_t kTlsClientHelloType = 0x30;
constexpr std::uint8_t kTlsServerHelloType = 0x31;
constexpr std::uint8_t kSecureEnvelopeType = 0x32;
constexpr std::uint8_t kSessionListRequestType = 0x07;
constexpr std::uint8_t kSessionListResponseType = 0x27;
constexpr std::uint8_t kStatsReportType = 0x28;
constexpr std::uint8_t kStatsAckType = 0x08;
constexpr std::uint8_t kStatsHistoryRequestType = 0x29;
constexpr std::uint8_t kStatsHistoryResponseType = 0x2A;
constexpr std::uint8_t kChatAckAction = 2;
constexpr std::uint8_t kChatReadAction = 3;
constexpr std::size_t kMaxStatsSamples = 64;
constexpr std::chrono::seconds kPresenceCooldown{2};

std::wstring BytesToHex(const std::vector<std::uint8_t>& data)
{
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(data.size() * 2);
    for (auto b : data)
    {
        out.push_back(digits[(b >> 4) & 0xF]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

std::vector<std::uint8_t> HexToBytes(const std::wstring& hex)
{
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0)
    {
        return out;
    }
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        auto hexVal = [](wchar_t c) -> int {
            if (c >= L'0' && c <= L'9')
                return c - L'0';
            if (c >= L'a' && c <= L'f')
                return 10 + (c - L'a');
            if (c >= L'A' && c <= L'F')
                return 10 + (c - L'A');
            return -1;
        };
        const int hi = hexVal(hex[i]);
        const int lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::wstring Utf8ToWide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

std::string WideToUtf8(const std::wstring& text)
{
    return std::string(text.begin(), text.end());
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
}  // namespace

namespace mi::server
{
MessageRouter::MessageRouter(AuthService& auth,
                             mi::shared::net::KcpChannel& channel,
                             std::vector<std::uint8_t> certBytes,
                             std::wstring certPassword,
                             std::string certFingerprint,
                             bool allowSelfSigned)
    : auth_(auth),
      channel_(channel),
      nextSessionId_(1),
      statePath_(L"server_state.csv"),
      certBytes_(std::move(certBytes)),
      certPassword_(std::move(certPassword)),
      certFingerprint_(std::move(certFingerprint)),
      allowSelfSigned_(allowSelfSigned),
      tlsReady_(false)
{
    LoadState();
    if (!certBytes_.empty())
    {
        const auto res = mi::shared::crypto::ValidatePfxChain(certBytes_, certPassword_, allowSelfSigned_);
        if (!certFingerprint_.empty() && !res.fingerprintHex.empty() && res.fingerprintHex != certFingerprint_)
        {
            std::wcerr << L"[router] 证书指纹不匹配，期望 " << Utf8ToWide(certFingerprint_) << L" 实际 "
                       << Utf8ToWide(res.fingerprintHex) << L"\n";
            tlsReady_ = false;
        }
        else if (res.ok)
        {
            tlsReady_ = true;
            std::wcout << L"[router] TLS 证书加载成功 subject=" << Utf8ToWide(res.subject) << L" issuer="
                       << Utf8ToWide(res.issuer) << L" 指纹=" << Utf8ToWide(res.fingerprintHex)
                       << (res.selfSigned ? L"（自签）" : L"") << L"\n";
        }
        else
        {
            std::wcerr << L"[router] 证书验证失败: " << res.error << L"\n";
        }
    }
}

void MessageRouter::HandleIncoming(const mi::shared::net::ReceivedDatagram& packet)
{
    const auto& sender = packet.sender;
    const auto& buffer = packet.payload;
    if (buffer.empty())
    {
        return;
    }

    std::uint8_t type = buffer[0];
    std::vector<std::uint8_t> payload(buffer.begin() + 1, buffer.end());
    if (type == kSecureEnvelopeType)
    {
        std::vector<std::uint8_t> inner;
        if (!DecryptEnvelope(packet.sessionId, payload, type, inner))
        {
            SendError(sender, 0x15, L"secure envelope decrypt failed", packet.sessionId);
            return;
        }
        payload = std::move(inner);
    }

    if (type == kTlsClientHelloType)
    {
        HandleTlsClientHello(payload, sender, packet.sessionId);
        return;
    }

    if (type == kAuthRequestType)
    {
        HandleAuth(payload, sender);
    }
    else if (type == kDataPacketType)
    {
        HandleData(payload, sender);
    }
    else if (type == kMediaChunkType)
    {
        HandleMediaChunk(payload, sender);
    }
    else if (type == kMediaControlType)
    {
        HandleMediaControl(payload, sender);
    }
    else if (type == kChatMessageType)
    {
        mi::shared::proto::ChatMessage msg{};
        if (!mi::shared::proto::ParseChatMessage(payload, msg))
        {
            SendError(sender, 0x09, L"chat parse failed");
            return;
        }
        if (msg.sessionId == 0 || !IsSenderAuthorized(msg.sessionId, sender))
        {
            SendError(sender, 0x05, L"session not registered for sender", msg.sessionId);
            return;
        }
       const std::uint32_t targetSession = (msg.targetSessionId != 0) ? msg.targetSessionId : msg.sessionId;
       const auto it = sessions_.find(targetSession);
       if (it == sessions_.end())
       {
           // 缓存离线消息，待目标上线推送
           offlineChats_[targetSession].push_back(msg);
           unreadCounts_[targetSession] += 1;
           SaveState();
           return;
       }
       std::vector<std::uint8_t> out;
       out.push_back(kChatMessageForwardType);
       const auto body = mi::shared::proto::SerializeChatMessage(msg);
       out.insert(out.end(), body.begin(), body.end());
       SendSecure(targetSession, it->second, out);
       unreadCounts_[targetSession] += 1;
       SaveState();
   }
    else if (type == kChatControlType)
    {
        mi::shared::proto::ChatControl ctl{};
        if (!mi::shared::proto::ParseChatControl(payload, ctl))
        {
            SendError(sender, 0x0A, L"chat control parse failed");
            return;
        }
        if (ctl.sessionId == 0 || !IsSenderAuthorized(ctl.sessionId, sender))
        {
            SendError(sender, 0x05, L"session not registered for sender", ctl.sessionId);
            return;
        }
        const std::uint32_t targetSession = (ctl.targetSessionId != 0) ? ctl.targetSessionId : ctl.sessionId;
        const auto it = sessions_.find(targetSession);
        if (it == sessions_.end())
        {
            SendError(sender, 0x06, L"target session not found", ctl.sessionId);
            return;
        }
        std::vector<std::uint8_t> out;
        out.push_back(kChatControlForwardType);
        const auto body = mi::shared::proto::SerializeChatControl(ctl);
        out.insert(out.end(), body.begin(), body.end());
        SendSecure(targetSession, it->second, out);
        if (ctl.action == kChatReadAction || ctl.action == kChatAckAction)
        {
            auto unreadIt = unreadCounts_.find(targetSession);
            if (unreadIt != unreadCounts_.end() && unreadIt->second > 0)
            {
                unreadIt->second = 0;
                SaveState();
            }
        }
        // 多端同步：将回执广播给订阅者
        for (const auto& kv : sessions_)
        {
            if (kv.first == targetSession || kv.first == ctl.sessionId)
            {
                continue;
            }
            SendSecure(kv.first, kv.second, out);
        }
    }
    else if (type == kStatsReportType)
    {
        mi::shared::proto::StatsReport rpt{};
        if (!mi::shared::proto::ParseStatsReport(payload, rpt))
        {
            SendError(sender, 0x0C, L"stats parse failed");
            return;
        }
        stats_[rpt.sessionId] = rpt;
        mi::shared::proto::StatsSample sample{};
        sample.sessionId = rpt.sessionId;
        sample.timestampSec = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        sample.stats = rpt;
        auto& vec = statsHistory_[rpt.sessionId];
        vec.push_back(sample);
        if (vec.size() > kMaxStatsSamples)
        {
            vec.erase(vec.begin());
        }
        SaveState();
        std::vector<std::uint8_t> out;
        out.push_back(kStatsAckType);
        SendSecure(rpt.sessionId, sender, out);
    }
    else if (type == kStatsHistoryRequestType)
    {
        mi::shared::proto::StatsHistoryRequest req{};
        if (!mi::shared::proto::ParseStatsHistoryRequest(payload, req))
        {
            SendError(sender, 0x0D, L"stats history parse failed");
            return;
        }
        if (req.sessionId == 0 || !IsSenderAuthorized(req.sessionId, sender))
        {
            SendError(sender, 0x05, L"session not registered for sender", req.sessionId);
            return;
        }
        mi::shared::proto::StatsHistoryResponse resp{};
        resp.sessionId = req.sessionId;
        resp.samples = GetStatsHistory(req.sessionId);
        if (resp.samples.empty())
        {
            const auto itStats = stats_.find(req.sessionId);
            if (itStats != stats_.end())
            {
                mi::shared::proto::StatsSample sample{};
                sample.sessionId = req.sessionId;
                sample.timestampSec = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                        .count());
                sample.stats = itStats->second;
                resp.samples.push_back(sample);
            }
        }
        std::vector<std::uint8_t> out;
        out.push_back(kStatsHistoryResponseType);
        const auto body = mi::shared::proto::SerializeStatsHistoryResponse(resp);
        out.insert(out.end(), body.begin(), body.end());
        SendSecure(req.sessionId, sender, out);
    }
    else if (type == kSessionListRequestType)
    {
        HandleSessionListRequest(payload, sender);
    }
    else
    {
        std::wcerr << L"[router] 未知消息类型: " << static_cast<int>(type) << L"\n";
        SendError(sender, 0x01, L"unsupported message type");
    }
}

std::uint32_t MessageRouter::ActiveSessions() const
{
    return static_cast<std::uint32_t>(sessions_.size());
}

std::vector<std::pair<std::uint32_t, mi::shared::net::PeerEndpoint>> MessageRouter::ListSessions() const
{
    std::vector<std::pair<std::uint32_t, mi::shared::net::PeerEndpoint>> out;
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_)
    {
        out.emplace_back(kv.first, kv.second);
    }
    return out;
}

std::vector<mi::shared::proto::SessionInfo> MessageRouter::GetSessionInfos() const
{
    std::vector<mi::shared::proto::SessionInfo> out;
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_)
    {
        mi::shared::proto::SessionInfo info{};
        info.sessionId = kv.first;
        info.peer = kv.second.host + L":" + std::to_wstring(kv.second.port);
        const auto unreadIt = unreadCounts_.find(kv.first);
        info.unreadCount = unreadIt != unreadCounts_.end() ? unreadIt->second : 0;
        out.push_back(info);
    }
    return out;
}

std::vector<mi::shared::proto::StatsSample> MessageRouter::GetStatsHistory(std::uint32_t sessionId) const
{
    const auto it = statsHistory_.find(sessionId);
    if (it == statsHistory_.end())
    {
        return {};
    }
    return it->second;
}

void MessageRouter::Tick()
{
    const auto active = channel_.ActiveSessionIds();
    const std::unordered_set<std::uint32_t> activeSet(active.begin(), active.end());
    bool removed = false;
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        if (activeSet.find(it->first) == activeSet.end())
        {
            std::wcout << L"[router] 会话 " << it->first << L" 不活跃，移除并广播\n";
            sessionSubscribers_.erase(it->first);
            unreadCounts_.erase(it->first);
            it = sessions_.erase(it);
            removed = true;
        }
        else
        {
            ++it;
        }
    }
    if (removed || !sessionSubscribers_.empty())
    {
        BroadcastSessionList();
        SaveState();
    }
}

void MessageRouter::HandleAuth(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender)
{
    mi::shared::proto::AuthRequest req{};
    if (!mi::shared::proto::ParseAuthRequest(buffer, req))
    {
        std::wcerr << L"[router] 解析认证请求失败\n";
        SendError(sender, 0x02, L"auth parse failed");
        return;
    }

    const bool ok = auth_.Validate(req.username, req.password);
    mi::shared::proto::AuthResponse resp{};
    resp.success = ok;
    if (ok)
    {
        resp.sessionId = nextSessionId_.FetchAndIncrement();
        if (resp.sessionId == 0)
        {
            resp.sessionId = nextSessionId_.Increment();  // 回退保护，避免 0 作为会话号
        }
    }

    if (resp.success)
    {
        mi::shared::net::Session session{};
        session.id = resp.sessionId;
        session.peer = sender;
        channel_.RegisterSession(session);
        sessions_[resp.sessionId] = sender;
        unreadCounts_[resp.sessionId] = 0;
        DeliverOffline(resp.sessionId);
    }

    std::vector<std::uint8_t> out;
    out.push_back(kAuthResponseType);
    const auto body = mi::shared::proto::SerializeAuthResponse(resp);
    out.insert(out.end(), body.begin(), body.end());

    SendSecure(resp.sessionId, sender, out);
    std::wcout << L"[router] 认证 " << (ok ? L"通过" : L"失败") << L" 用户=" << req.username << L"\n";
    if (resp.success)
    {
        BroadcastSessionList();
    }
}

void MessageRouter::HandleData(const std::vector<std::uint8_t>& buffer, const mi::shared::net::PeerEndpoint& sender)
{
    mi::shared::proto::DataPacket pkt{};
    if (!mi::shared::proto::ParseDataPacket(buffer, pkt))
    {
        std::wcerr << L"[router] 解析数据包失败\n";
        SendError(sender, 0x03, L"data parse failed");
        return;
    }

    if (pkt.sessionId == 0)
    {
        SendError(sender, 0x04, L"missing session");
        return;
    }

    if (!IsSenderAuthorized(pkt.sessionId, sender))
    {
        SendError(sender, 0x05, L"session not registered for sender", pkt.sessionId);
        return;
    }

    const std::uint32_t targetSession = (pkt.targetSessionId != 0) ? pkt.targetSessionId : pkt.sessionId;
    const auto it = sessions_.find(targetSession);
    if (it == sessions_.end())
    {
        SendError(sender, 0x06, L"target session not found", pkt.sessionId);
        return;
    }
    const mi::shared::net::PeerEndpoint target = it->second;

    std::vector<std::uint8_t> out;
    out.push_back(kDataForwardType);
    const auto body = mi::shared::proto::SerializeDataPacket(pkt);
    out.insert(out.end(), body.begin(), body.end());
    SendSecure(targetSession, target, out);

    std::wcout << L"[router] 转发数据 session=" << pkt.sessionId << L" -> " << targetSession << L" 长度=" << pkt.payload.size()
               << L" 来自 " << sender.host << L":" << sender.port << L"\n";
}

void MessageRouter::HandleMediaChunk(const std::vector<std::uint8_t>& buffer,
                                     const mi::shared::net::PeerEndpoint& sender)
{
    mi::shared::proto::MediaChunk pkt{};
    if (!mi::shared::proto::ParseMediaChunk(buffer, pkt))
    {
        std::wcerr << L"[router] 解析媒体分片失败\n";
        SendError(sender, 0x07, L"media parse failed");
        return;
    }

    if (pkt.sessionId == 0 || !IsSenderAuthorized(pkt.sessionId, sender))
    {
        SendError(sender, 0x05, L"session not registered for sender", pkt.sessionId);
        return;
    }

    const std::uint32_t targetSession = (pkt.targetSessionId != 0) ? pkt.targetSessionId : pkt.sessionId;
    const auto it = sessions_.find(targetSession);
    if (it == sessions_.end())
    {
        SendError(sender, 0x06, L"target session not found", pkt.sessionId);
        return;
    }
    const mi::shared::net::PeerEndpoint target = it->second;

    std::vector<std::uint8_t> out;
    out.push_back(kMediaForwardType);
    const auto body = mi::shared::proto::SerializeMediaChunk(pkt);
    out.insert(out.end(), body.begin(), body.end());
    SendSecure(targetSession, target, out);
}

void MessageRouter::HandleMediaControl(const std::vector<std::uint8_t>& buffer,
                                       const mi::shared::net::PeerEndpoint& sender)
{
    mi::shared::proto::MediaControl ctl{};
    if (!mi::shared::proto::ParseMediaControl(buffer, ctl))
    {
        SendError(sender, 0x08, L"media control parse failed");
        return;
    }

    if (ctl.sessionId == 0 || !IsSenderAuthorized(ctl.sessionId, sender))
    {
        SendError(sender, 0x05, L"session not registered for sender", ctl.sessionId);
        return;
    }

    const std::uint32_t targetSession = (ctl.targetSessionId != 0) ? ctl.targetSessionId : ctl.sessionId;
    const auto it = sessions_.find(targetSession);
    if (it == sessions_.end())
    {
        SendError(sender, 0x06, L"target session not found", ctl.sessionId);
        return;
    }
    const mi::shared::net::PeerEndpoint target = it->second;

    std::vector<std::uint8_t> out;
    out.push_back(kMediaControlForwardType);
    const auto body = mi::shared::proto::SerializeMediaControl(ctl);
    out.insert(out.end(), body.begin(), body.end());
    SendSecure(targetSession, target, out);
}

void MessageRouter::HandleSessionListRequest(const std::vector<std::uint8_t>& buffer,
                                             const mi::shared::net::PeerEndpoint& sender)
{
    mi::shared::proto::SessionListRequest req{};
    if (!mi::shared::proto::ParseSessionListRequest(buffer, req))
    {
        SendError(sender, 0x0B, L"session list parse failed");
        return;
    }
    if (req.sessionId == 0 || !IsSenderAuthorized(req.sessionId, sender))
    {
        SendError(sender, 0x05, L"session not registered for sender", req.sessionId);
        return;
    }
    if (req.subscribe)
    {
        sessionSubscribers_.insert(req.sessionId);
    }
    static std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> lastPing;
    const auto now = std::chrono::steady_clock::now();
    const auto itPing = lastPing.find(req.sessionId);
    if (itPing != lastPing.end() && now - itPing->second < kPresenceCooldown)
    {
        return;
    }
    lastPing[req.sessionId] = now;
    SendSessionList(sender, req.sessionId, req.subscribe);
}

void MessageRouter::SendError(const mi::shared::net::PeerEndpoint& target,
                              std::uint8_t code,
                              const std::wstring& message,
                              std::uint32_t sessionIdHint)
{
    mi::shared::proto::ErrorResponse error{};
    error.code = code;
    error.message = message;
    std::vector<std::uint8_t> out;
    out.push_back(kErrorType);
    const auto body = mi::shared::proto::SerializeErrorResponse(error);
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t sessionId = sessionIdHint;
    if (sessionId == 0)
    {
        sessionId = channel_.FindSessionId(target);
    }
    SendSecure(sessionId, target, out);
}

void MessageRouter::BroadcastSessionList()
{
    std::vector<std::uint32_t> stale;
    for (std::uint32_t sid : sessionSubscribers_)
    {
        const auto it = sessions_.find(sid);
        if (it == sessions_.end())
        {
            stale.push_back(sid);
            continue;
        }
        SendSessionList(it->second, sid, true);
    }
    for (std::uint32_t sid : stale)
    {
        sessionSubscribers_.erase(sid);
    }
    // 触发离线订阅者的离线消息投递
    for (auto& kv : sessions_)
    {
        DeliverOffline(kv.first);
    }
}

void MessageRouter::LoadState()
{
    std::wifstream in(statePath_.c_str());
    if (!in.is_open())
    {
        return;
    }
    std::wstring line;
    while (std::getline(in, line))
    {
        if (line.empty())
        {
            continue;
        }
        std::wstringstream ss(line);
        wchar_t type;
        ss >> type;
        if (type == L'u')
        {
            std::uint32_t sid = 0;
            std::uint32_t unread = 0;
            wchar_t comma;
            ss >> comma >> sid >> comma >> unread;
            if (sid != 0)
            {
                unreadCounts_[sid] = unread;
            }
        }
        else if (type == L's')
        {
            std::wstring rest;
            if (!std::getline(ss, rest))
            {
                continue;
            }
            std::wstringstream parts(rest);
            std::vector<std::wstring> tokens;
            std::wstring token;
            while (std::getline(parts, token, L','))
            {
                if (!token.empty())
                {
                    tokens.push_back(token);
                }
            }
            if (tokens.size() < 6)
            {
                continue;
            }
            try
            {
                mi::shared::proto::StatsReport rpt{};
                rpt.sessionId = std::stoul(tokens[0]);
                rpt.bytesSent = std::stoull(tokens[1]);
                rpt.bytesReceived = std::stoull(tokens[2]);
                rpt.chatFailures = std::stoul(tokens[3]);
                rpt.dataFailures = std::stoul(tokens[4]);
                rpt.mediaFailures = std::stoul(tokens[5]);
                rpt.durationMs = tokens.size() > 6 ? std::stoul(tokens[6]) : 0;
                const std::uint32_t ts = tokens.size() > 7 ? static_cast<std::uint32_t>(std::stoul(tokens[7])) : 0;
                if (rpt.sessionId != 0)
                {
                    stats_[rpt.sessionId] = rpt;
                    mi::shared::proto::StatsSample sample{};
                    sample.sessionId = rpt.sessionId;
                    sample.timestampSec = ts;
                    sample.stats = rpt;
                    statsHistory_[rpt.sessionId].push_back(sample);
                }
            }
            catch (const std::exception&)
            {
                continue;
            }
        }
        else if (type == L'h')
        {
            // h,sessionId,timestampSec,bytesSent,bytesReceived,chatFailures,dataFailures,mediaFailures,durationMs
            std::wstring rest;
            if (!std::getline(ss, rest))
            {
                continue;
            }
            std::wstringstream parts(rest);
            std::vector<std::wstring> tokens;
            std::wstring token;
            while (std::getline(parts, token, L','))
            {
                if (!token.empty())
                {
                    tokens.push_back(token);
                }
            }
            if (tokens.size() < 6)
            {
                continue;
            }
            try
            {
                mi::shared::proto::StatsSample sample{};
                sample.sessionId = std::stoul(tokens[0]);
                sample.timestampSec = static_cast<std::uint32_t>(std::stoul(tokens[1]));
                sample.stats.sessionId = sample.sessionId;
                sample.stats.bytesSent = std::stoull(tokens[2]);
                sample.stats.bytesReceived = std::stoull(tokens[3]);
                sample.stats.chatFailures = std::stoul(tokens[4]);
                sample.stats.dataFailures = std::stoul(tokens[5]);
                sample.stats.mediaFailures = tokens.size() > 6 ? std::stoul(tokens[6]) : 0;
                sample.stats.durationMs = tokens.size() > 7 ? std::stoul(tokens[7]) : 0;
                auto& vec = statsHistory_[sample.sessionId];
                vec.push_back(sample);
                if (vec.size() > kMaxStatsSamples)
                {
                    vec.erase(vec.begin());
                }
                stats_[sample.sessionId] = sample.stats;
            }
            catch (const std::exception&)
            {
                continue;
            }
        }
        else if (type == L'o')
        {
            // o,sessionId,targetSession,messageId,format,attCount,attHex...,payloadHex
            wchar_t comma;
            mi::shared::proto::ChatMessage msg{};
            std::uint32_t targetSession = 0;
            std::uint32_t attCount = 0;
            std::wstring payloadHex;
            ss >> comma >> msg.sessionId >> comma >> targetSession >> comma >> msg.messageId >> comma >> attCount >> comma;
            std::wstring attToken;
            for (std::uint32_t i = 0; i < attCount; ++i)
            {
                if (!std::getline(ss, attToken, L','))
                {
                    break;
                }
                msg.attachments.push_back(Utf8ToWide(WideToUtf8(attToken)));
            }
            if (std::getline(ss, payloadHex))
            {
                msg.payload = HexToBytes(payloadHex);
            }
            offlineChats_[targetSession].push_back(msg);
        }
    }
}

void MessageRouter::SaveState() const
{
    std::wofstream out(statePath_.c_str(), std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::uint32_t nowSec = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    for (const auto& kv : unreadCounts_)
    {
        out << L"u," << kv.first << L"," << kv.second << L"\n";
    }
    for (const auto& kv : stats_)
    {
        const std::uint32_t ts = nowSec;
        out << L"s," << kv.first << L"," << kv.second.bytesSent << L"," << kv.second.bytesReceived << L","
            << kv.second.chatFailures << L"," << kv.second.dataFailures << L"," << kv.second.mediaFailures << L","
            << kv.second.durationMs << L"," << ts << L"\n";
    }
    for (const auto& kv : statsHistory_)
    {
        for (const auto& sample : kv.second)
        {
            out << L"h," << sample.sessionId << L"," << sample.timestampSec << L"," << sample.stats.bytesSent << L","
                << sample.stats.bytesReceived << L"," << sample.stats.chatFailures << L"," << sample.stats.dataFailures << L","
                << sample.stats.mediaFailures << L"," << sample.stats.durationMs << L"\n";
        }
    }
    for (const auto& kv : offlineChats_)
    {
        for (const auto& msg : kv.second)
        {
            out << L"o," << msg.sessionId << L"," << kv.first << L"," << msg.messageId << L"," << msg.attachments.size()
                << L",";
            for (const auto& att : msg.attachments)
            {
                out << Utf8ToWide(WideToUtf8(att)) << L",";
            }
            out << BytesToHex(msg.payload) << L"\n";
        }
    }
}

void MessageRouter::SendSessionList(const mi::shared::net::PeerEndpoint& target,
                                    std::uint32_t sessionId,
                                    bool subscribed)
{
    mi::shared::proto::SessionListResponse resp{};
    resp.subscribed = subscribed;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    resp.serverTimeSec = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    resp.sessions.reserve(sessions_.size());
    for (const auto& kv : sessions_)
    {
        mi::shared::proto::SessionInfo info{};
        info.sessionId = kv.first;
        info.peer = kv.second.host + L":" + std::to_wstring(kv.second.port);
        auto unreadIt = unreadCounts_.find(kv.first);
        info.unreadCount = unreadIt != unreadCounts_.end() ? unreadIt->second : 0;
        resp.sessions.push_back(std::move(info));
    }
    std::vector<std::uint8_t> out;
    out.push_back(kSessionListResponseType);
    const auto body = mi::shared::proto::SerializeSessionListResponse(resp);
    out.insert(out.end(), body.begin(), body.end());
    SendSecure(sessionId, target, out);
}

mi::shared::crypto::WhiteboxKeyInfo MessageRouter::BuildTlsKey(const std::vector<std::uint8_t>& secret) const
{
    mi::shared::crypto::WhiteboxKeyInfo info{};
    info.keyParts = secret;
    if (info.keyParts.empty())
    {
        info.keyParts.push_back(0x5Au);
    }
    return info;
}

void MessageRouter::SendSecure(std::uint32_t sessionId,
                               const mi::shared::net::PeerEndpoint& peer,
                               const std::vector<std::uint8_t>& plain)
{
    auto it = tlsKeys_.find(sessionId);
    if (it != tlsKeys_.end())
    {
        const auto cipher = mi::shared::crypto::Encrypt(plain, it->second);
        std::vector<std::uint8_t> env;
        env.push_back(kSecureEnvelopeType);
        env.insert(env.end(), cipher.begin(), cipher.end());
        channel_.Send(peer, env, sessionId);
        return;
    }
    channel_.Send(peer, plain, sessionId);
}

bool MessageRouter::DecryptEnvelope(std::uint32_t sessionId,
                                    const std::vector<std::uint8_t>& cipher,
                                    std::uint8_t& innerType,
                                    std::vector<std::uint8_t>& innerPayload)
{
    auto it = tlsKeys_.find(sessionId);
    if (it == tlsKeys_.end())
    {
        return false;
    }
    const auto plain = mi::shared::crypto::Decrypt(cipher, it->second);
    if (plain.empty())
    {
        return false;
    }
    innerType = plain[0];
    innerPayload.assign(plain.begin() + 1, plain.end());
    return true;
}

void MessageRouter::HandleTlsClientHello(const std::vector<std::uint8_t>& buffer,
                                         const mi::shared::net::PeerEndpoint& sender,
                                         std::uint32_t sessionIdHint)
{
    if (!tlsReady_ || certBytes_.empty())
    {
        SendError(sender, 0x16, L"tls not ready", sessionIdHint);
        return;
    }
    if (buffer.size() <= 4)
    {
        SendError(sender, 0x17, L"bad tls hello", sessionIdHint);
        return;
    }
    const std::uint32_t sid = ReadLe32(buffer, 0);
    const std::uint32_t effectiveSid = sessionIdHint != 0 ? sessionIdHint : sid;
    if (!IsSenderAuthorized(effectiveSid, sender))
    {
        SendError(sender, 0x18, L"unauthorized for tls", effectiveSid);
        return;
    }
    std::vector<std::uint8_t> enc(buffer.begin() + 4, buffer.end());
    std::vector<std::uint8_t> secret;
    if (!mi::shared::crypto::DecryptWithPrivateKey(certBytes_, certPassword_, enc, secret) || secret.empty())
    {
        SendError(sender, 0x19, L"tls decrypt failed", effectiveSid);
        return;
    }
    tlsKeys_[effectiveSid] = BuildTlsKey(secret);
    const auto hash = mi::shared::crypto::Sha256(secret);
    std::vector<std::uint8_t> ack;
    ack.push_back(kTlsServerHelloType);
    WriteLe32(ack, effectiveSid);
    ack.insert(ack.end(), hash.begin(), hash.end());
    channel_.Send(sender, ack, effectiveSid);
    std::wcout << L"[router] 会话 " << effectiveSid << L" TLS 握手完成\n";
}

bool MessageRouter::IsSenderAuthorized(std::uint32_t sessionId, const mi::shared::net::PeerEndpoint& sender)
{
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
    {
        return false;
    }

    if (it->second.host == sender.host && it->second.port == sender.port)
    {
        return true;
    }

    if (it->second.host == sender.host)
    {
        // 允许同一主机端口漂移场景下自动重绑
        mi::shared::net::Session session{};
        session.id = sessionId;
        session.peer = sender;
        channel_.RegisterSession(session);
        it->second = sender;
        std::wcout << L"[router] 会话 " << sessionId << L" 端点更新为 " << sender.host << L":" << sender.port << L"\n";
        BroadcastSessionList();
        DeliverOffline(sessionId);
        return true;
    }
    return false;
}

void MessageRouter::DeliverOffline(std::uint32_t sessionId)
{
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end())
    {
        return;
    }
    auto offIt = offlineChats_.find(sessionId);
    if (offIt == offlineChats_.end())
    {
        return;
    }
    for (const auto& msg : offIt->second)
    {
        std::vector<std::uint8_t> out;
        out.push_back(kChatMessageForwardType);
        const auto body = mi::shared::proto::SerializeChatMessage(msg);
        out.insert(out.end(), body.begin(), body.end());
        SendSecure(sessionId, it->second, out);
    }
    unreadCounts_[sessionId] += static_cast<std::uint32_t>(offIt->second.size());
    offlineChats_.erase(offIt);
}
}  // namespace mi::server
