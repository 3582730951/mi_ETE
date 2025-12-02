#include "server/server_app.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>
#include <utility>
#include <unordered_map>
#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "server/message_router.hpp"
#include "mi/shared/crypto/cert_store.hpp"
#include "mi/shared/crypto/tls_support.hpp"

namespace mi::server
{
ServerApplication::ServerApplication(ServerConfig config)
    : config_(std::move(config)), channel_(), running_(false), auth_(config_.allowedUsers), panel_(), router_(nullptr)
{
}

namespace
{
std::string ToUtf8(const std::wstring& text)
{
#ifdef _WIN32
    if (text.empty())
    {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
#else
    std::string utf8;
    utf8.reserve(text.size());
    for (wchar_t wc : text)
    {
        utf8.push_back(static_cast<char>(wc <= 0x7F ? wc : '?'));
    }
    return utf8;
#endif
}

std::string Narrow(const std::wstring& text)
{
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text)
    {
        out.push_back(static_cast<char>(ch & 0xFF));
    }
    return out;
}

std::unordered_map<std::string, std::string> ParseQuery(const std::string& query)
{
    std::unordered_map<std::string, std::string> params;
    std::size_t start = 0;
    while (start < query.size())
    {
        const auto eq = query.find('=', start);
        if (eq == std::string::npos)
        {
            break;
        }
        const auto amp = query.find('&', eq + 1);
        const std::string key = query.substr(start, eq - start);
        const std::string val = query.substr(eq + 1, amp == std::string::npos ? std::string::npos : amp - eq - 1);
        params[key] = val;
        if (amp == std::string::npos)
        {
            break;
        }
        start = amp + 1;
    }
    return params;
}

std::string Base64Encode(const std::vector<std::uint8_t>& data)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int valb = -6;
    for (std::uint8_t c : data)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            out.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
    {
        out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (out.size() % 4)
    {
        out.push_back('=');
    }
    return out;
}
}  // namespace

void ServerApplication::RefreshPanelCache()
{
    std::ostringstream oss;
    const uint32_t sessions = router_ ? router_->ActiveSessions() : 0;
    const uint16_t port = channel_.BoundPort();
    const auto now = std::chrono::steady_clock::now();
    const auto uptimeSec = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_).count();
    const auto stats = channel_.CollectStats();
    const auto list = router_ ? router_->ListSessions() : std::vector<std::pair<std::uint32_t, mi::shared::net::PeerEndpoint>>{};

    oss << "{\"sessions\":" << sessions << ",\"port\":" << port << ",\"uptime_sec\":" << uptimeSec << ",\"list\":[";
    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& item = list[i];
        const std::string peerUtf8 = ToUtf8(item.second.host);
        oss << "{\"id\":" << item.first << ",\"peer\":\"" << peerUtf8 << ":" << item.second.port << "\"}";
        if (i + 1 < list.size())
        {
            oss << ",";
        }
    }
    oss << "]";

    oss << ",\"kcp\":{\"session_count\":" << stats.sessionCount << ",\"crc_ok\":" << stats.crcOk << ",\"crc_fail\":"
        << stats.crcFail << ",\"idle_reclaimed\":" << stats.idleReclaimed << ",\"mtu\":" << channel_.Settings().mtu
        << ",\"interval_ms\":" << channel_.Settings().intervalMs << "}";

    if (!config_.panelToken.empty())
    {
        oss << ",\"auth\":\"required\"";
    }
    if (channel_.Settings().enableCrc32)
    {
        oss << ",\"crc\":{\"enabled\":true,\"max_frame\":" << channel_.Settings().maxFrameSize << "}";
    }
    oss << "}";

    {
        std::lock_guard<std::mutex> lock(panelMutex_);
        panelCache_ = oss.str();
        lastPanelRefresh_ = now;
    }
}

std::string ServerApplication::GetPanelCache()
{
    std::lock_guard<std::mutex> lock(panelMutex_);
    return panelCache_;
}

std::string ServerApplication::GetCertBase64() const
{
    // 优先配置文件，其次环境变量，均不落地
    if (!config_.certBase64.empty())
    {
        return Narrow(config_.certBase64);
    }
    const char* env = std::getenv("MI_CERT_B64");
    if (env != nullptr)
    {
        return std::string(env);
    }
    return {};
}

std::string ServerApplication::GetCertPassword() const
{
    if (!config_.certPassword.empty())
    {
        return Narrow(config_.certPassword);
    }
    const char* env = std::getenv("MI_CERT_PWD");
    if (env != nullptr)
    {
        return std::string(env);
    }
    return {};
}

static std::string ToLowerHex(const std::vector<std::uint8_t>& data)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t b : data)
    {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

std::string ServerApplication::HandlePanelPath(const std::string& path)
{
    std::string route = path.empty() ? "/" : path;
    std::string query;
    const auto qpos = route.find('?');
    if (qpos != std::string::npos)
    {
        query = route.substr(qpos + 1);
        route = route.substr(0, qpos);
    }
    if (route == "/" || route == "/panel")
    {
        return GetPanelCache();
    }
    if (route == "/sessions")
    {
        if (!router_)
        {
            return GetPanelCache();
        }
        const auto infos = router_->GetSessionInfos();
        std::ostringstream oss;
        oss << "{\"sessions\":[";
        for (size_t i = 0; i < infos.size(); ++i)
        {
            const auto& info = infos[i];
            oss << "{\"id\":" << info.sessionId << ",\"name\":\"" << ToUtf8(info.peer) << "\",\"online\":true,"
                << "\"unread\":" << info.unreadCount << "}";
            if (i + 1 < infos.size())
            {
                oss << ",";
            }
        }
        oss << "]}";
        return oss.str();
    }
    if (route == "/stats")
    {
        if (!router_)
        {
            return GetPanelCache();
        }
        const auto params = ParseQuery(query);
        std::uint32_t sessionId = 0;
        try
        {
            auto it = params.find("session");
            if (it != params.end())
            {
                sessionId = static_cast<std::uint32_t>(std::stoul(it->second));
            }
            else if ((it = params.find("sessionId")) != params.end())
            {
                sessionId = static_cast<std::uint32_t>(std::stoul(it->second));
            }
        }
        catch (const std::exception&)
        {
            return "{\"error\":\"bad_session\"}";
        }
        if (sessionId == 0)
        {
            return "{\"error\":\"missing_session\"}";
        }
        const auto hist = router_->GetStatsHistory(sessionId);
        std::ostringstream oss;
        oss << "{\"sessionId\":" << sessionId << ",\"samples\":[";
        for (size_t i = 0; i < hist.size(); ++i)
        {
            const auto& s = hist[i];
            oss << "{\"ts\":" << s.timestampSec << ",\"sent\":" << s.stats.bytesSent << ",\"recv\":" << s.stats.bytesReceived
                << ",\"chat_fail\":" << s.stats.chatFailures << ",\"data_fail\":" << s.stats.dataFailures
                << ",\"media_fail\":" << s.stats.mediaFailures << ",\"dur\":" << s.stats.durationMs << "}";
            if (i + 1 < hist.size())
            {
                oss << ",";
            }
        }
        oss << "]}";
        return oss.str();
    }
    if (route == "/cert")
    {
        const auto cert = GetCertBase64();
        if (cert.empty())
        {
            return "{\"error\":\"cert_missing\"}";
        }
        std::string sha;
        if (!config_.certSha256.empty())
        {
            sha = Narrow(config_.certSha256);
        }
        else
        {
            const auto bytes = mi::shared::crypto::LoadCertFromBase64(cert);
            const auto hex = mi::shared::crypto::Sha256Hex(bytes);
            sha = hex;
        }
        std::ostringstream oss;
        oss << "{\"cert\":\"" << cert << "\"";
        const auto pwd = GetCertPassword();
        if (!pwd.empty())
        {
            oss << ",\"password\":\"" << pwd << "\"";
        }
        if (!sha.empty())
        {
            oss << ",\"sha256\":\"" << sha << "\"";
        }
        oss << ",\"allowSelfSigned\":" << (config_.certAllowSelfSigned ? "true" : "false");
        oss << "}";
        return oss.str();
    }
    return {};
}

bool ServerApplication::Start()
{
    InitializeChannel();
    if (!channel_.Start(config_.listenHost, config_.listenPort))
    {
        std::wcerr << L"[server] KCP 通道启动失败\n";
        return false;
    }

    const auto certBase64 = GetCertBase64();
    const auto certPassword = GetCertPassword();
    std::vector<std::uint8_t> certBytes;
    if (!certBase64.empty())
    {
        certBytes = mi::shared::crypto::LoadCertFromBase64(certBase64);
    }
    std::wstring certPwdW;
    if (!certPassword.empty())
    {
        certPwdW.assign(certPassword.begin(), certPassword.end());
    }
    std::string certFingerprint;
    if (!config_.certSha256.empty())
    {
        certFingerprint.assign(config_.certSha256.begin(), config_.certSha256.end());
    }
    router_ = std::make_unique<MessageRouter>(auth_, channel_, certBytes, certPwdW, certFingerprint, config_.certAllowSelfSigned);
    startTime_ = std::chrono::steady_clock::now();
    RefreshPanelCache();
    const PanelResponder responder = [this](const std::string& path) -> std::string { return HandlePanelPath(path); };

    if (!panel_.Start(config_.panelHost, config_.panelPort, responder, config_.panelToken))
    {
        std::wcerr << L"[server] 面板启动失败\n";
        return false;
    }

    running_.store(true);
    std::wcout << L"[server] 已启动，监听 " << config_.listenHost << L":" << config_.listenPort << L"\n";
    return true;
}

void ServerApplication::RunOnce()
{
    if (!running_.load())
    {
        return;
    }

    channel_.Poll();
    RefreshPanelCache();
}

void ServerApplication::Run()
{
    if (!running_.load())
    {
        return;
    }

    while (running_.load())
    {
        channel_.Poll();
        mi::shared::net::ReceivedDatagram packet{};
        while (channel_.TryReceive(packet))
        {
            if (router_)
            {
                router_->HandleIncoming(packet);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPanelRefresh_ > std::chrono::seconds(1))
        {
            RefreshPanelCache();
            if (router_)
            {
                router_->Tick();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.pollSleepMs));
    }
}

void ServerApplication::Stop()
{
    if (!running_.load())
    {
        return;
    }

    panel_.Stop();
    channel_.Stop();
    running_.store(false);
    std::wcout << L"[server] 已停止\n";
}

bool ServerApplication::IsRunning() const
{
    return running_.load();
}

void ServerApplication::InitializeChannel()
{
    mi::shared::net::KcpSettings settings{};
    settings.intervalMs = config_.kcpIntervalMs;
    settings.mtu = config_.kcpMtu;
    settings.sendWindow = config_.kcpSendWindow;
    settings.receiveWindow = config_.kcpRecvWindow;
    settings.idleTimeoutMs = config_.kcpIdleTimeoutMs;
    settings.peerRebindCooldownMs = config_.kcpPeerRebindMs;
    settings.enableCrc32 = config_.kcpCrcEnable;
    settings.crcDropLog = config_.kcpCrcDropLog;
    settings.maxFrameSize = config_.kcpMaxFrameSize;
    channel_.Configure(settings);
}
}  // namespace mi::server
