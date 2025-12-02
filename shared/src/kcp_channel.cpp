#include "mi/shared/net/kcp_channel.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

#include "ikcp.h"

#ifdef _WIN32
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace
{
constexpr std::size_t kIkcpOverhead = 24;
#pragma pack(push, 1)
struct UdpFrame
{
    std::uint8_t magic = 0x5Au;
    std::uint8_t flags = 0;
    std::uint16_t length = 0;
    std::uint32_t sessionId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint32_t crc = 0;
};
#pragma pack(pop)

std::uint32_t Crc32(const std::uint8_t* data, std::size_t len)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
        {
            const std::uint32_t lsb = crc & 1u;
            crc = (crc >> 1) ^ (lsb ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}

bool ValidateFrame(const std::vector<std::uint8_t>& buffer, bool logOnFailure)
{
    if (buffer.size() < sizeof(UdpFrame))
    {
        return false;
    }
    const UdpFrame* frame = reinterpret_cast<const UdpFrame*>(buffer.data());
    if (frame->magic != 0x5Au)
    {
        return false;
    }
    const std::uint16_t length = frame->length;
    if (length + sizeof(UdpFrame) != buffer.size())
    {
        return false;
    }
    const std::uint32_t expected = Crc32(buffer.data(), buffer.size() - sizeof(frame->crc));
    const bool ok = expected == frame->crc;
    if (!ok && logOnFailure)
    {
        std::wcerr << L"[kcp] CRC 校验失败 magic=" << static_cast<int>(frame->magic) << L" length=" << length << L" sid="
                   << frame->sessionId << L" recvCrc=" << frame->crc << L" calc=" << expected << L"\n";
    }
    return ok;
}

std::vector<std::uint8_t> WrapFrame(const std::vector<std::uint8_t>& payload, std::uint32_t sessionId)
{
    UdpFrame header{};
    header.magic = 0x5Au;
    header.flags = 0;
    header.length = static_cast<std::uint16_t>(payload.size());
    header.sessionId = sessionId;
    header.sequence = 0;
    header.ack = 0;

    std::vector<std::uint8_t> buffer(sizeof(UdpFrame) + payload.size(), 0);
    std::memcpy(buffer.data(), &header, sizeof(UdpFrame));
    std::memcpy(buffer.data() + sizeof(UdpFrame), payload.data(), payload.size());
    const std::uint32_t crc = Crc32(buffer.data(), buffer.size() - sizeof(header.crc));
    std::memcpy(buffer.data() + sizeof(UdpFrame) - sizeof(header.crc), &crc, sizeof(crc));
    return buffer;
}

#ifdef _WIN32
std::wstring ToWideMessage(DWORD errorCode)
{
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        errorCode,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);
    if (size == 0 || buffer == nullptr)
    {
        return L"unknown";
    }

    std::wstring msg(buffer, size);
    ::LocalFree(buffer);
    return msg;
}
#endif
}  // namespace

namespace mi::shared::net
{
KcpChannel::KcpChannel()
    : settings_{},
      running_(false),
      socketHandle_(0),
      winsockReady_(false),
      boundPort_(0),
      received_{},
      lastReceived_{},
      lastSender_{},
      sessions_{},
      peerToSession_{},
      reclaimedCount_(0)
{
}

KcpChannel::~KcpChannel()
{
    Stop();
}

void KcpChannel::Configure(const KcpSettings& settings)
{
    settings_ = settings;
}

bool KcpChannel::Start(const std::wstring& host, uint16_t port)
{
#ifdef _WIN32
    if (!winsockReady_)
    {
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            std::wcerr << L"[kcp] WSAStartup 失败\n";
            return false;
        }
        winsockReady_ = true;
    }

    SOCKET sock = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET)
    {
        std::wcerr << L"[kcp] 创建 UDP socket 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        return false;
    }

    u_long nonBlocking = 1;
    ::ioctlsocket(sock, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::InetPtonW(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        std::wcerr << L"[kcp] 解析地址失败: " << host << L"\n";
        ::closesocket(sock);
        return false;
    }

    if (::bind(sock, reinterpret_cast<SOCKADDR*>(&addr), static_cast<int>(sizeof(addr))) == SOCKET_ERROR)
    {
        std::wcerr << L"[kcp] 绑定失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        ::closesocket(sock);
        return false;
    }

    sockaddr_in boundAddr{};
    int addrLen = sizeof(boundAddr);
    if (::getsockname(sock, reinterpret_cast<SOCKADDR*>(&boundAddr), &addrLen) == 0)
    {
        boundPort_ = ntohs(boundAddr.sin_port);
    }
    else
    {
        boundPort_ = port;
    }

    socketHandle_ = static_cast<std::uintptr_t>(sock);
    running_ = true;
    std::wcout << L"[kcp] 监听 " << host << L":" << boundPort_ << L" mtu=" << settings_.mtu << L" interval=" << settings_.intervalMs
               << L"ms\n";
    return true;
#else
    std::wcerr << L"[kcp] 非 Windows 平台暂未实现\n";
    return false;
#endif
}

bool KcpChannel::Send(const PeerEndpoint& peer, const std::vector<std::uint8_t>& payload, std::uint32_t sessionId)
{
    if (!running_)
    {
        std::wcerr << L"[kcp] 未启动，无法发送\n" << std::flush;
        return false;
    }

    const std::uint32_t now = NowMs();
    SessionState& state = EnsureSession(sessionId, peer);
    UpdatePeer(sessionId, state, peer, now);
    if (state.kcp == nullptr)
    {
        std::wcerr << L"[kcp] 创建 KCP 会话失败\n" << std::flush;
        return false;
    }

    const int ret = ikcp_send(state.kcp, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()));
    if (ret < 0)
    {
        std::wcerr << L"[kcp] ikcp_send 失败: " << ret << L"\n" << std::flush;
        return false;
    }
    state.lastSendMs = now;
    state.lastActiveMs = now;
    ikcp_flush(state.kcp);
    return true;
}

void KcpChannel::Poll()
{
    if (!running_)
    {
        return;
    }

    ProcessIncoming();
    UpdateSessions();
}

bool KcpChannel::TryReceive(ReceivedDatagram& packet)
{
    if (received_.empty())
    {
        return false;
    }

    packet = received_.front();
    received_.pop_front();
    lastReceived_ = packet.payload;
    lastSender_ = packet.sender;
    return true;
}

void KcpChannel::Stop()
{
    if (!running_)
    {
        Reset();
        return;
    }

#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(socketHandle_);
    if (sock != INVALID_SOCKET)
    {
        ::closesocket(sock);
    }
#endif
    running_ = false;
    std::wcout << L"[kcp] 已停止\n";
    Reset();
}

bool KcpChannel::IsRunning() const
{
    return running_;
}

KcpSettings KcpChannel::Settings() const
{
    return settings_;
}

std::vector<std::uint8_t> KcpChannel::LastReceived() const
{
    return lastReceived_;
}

PeerEndpoint KcpChannel::LastSender() const
{
    return lastSender_;
}

void KcpChannel::RegisterSession(const Session& session)
{
    SessionState& state = EnsureSession(session.id, session.peer);
    state.peer = session.peer;
}

PeerEndpoint KcpChannel::FindPeer(std::uint32_t sessionId) const
{
    const auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
    {
        return it->second.peer;
    }
    return PeerEndpoint{};
}

std::uint32_t KcpChannel::FindSessionId(const PeerEndpoint& peer) const
{
    const auto it = peerToSession_.find(BuildPeerKey(peer));
    if (it != peerToSession_.end())
    {
        return it->second;
    }
    return 0;
}

uint16_t KcpChannel::BoundPort() const
{
    return boundPort_;
}

KcpChannelStats KcpChannel::CollectStats() const
{
    KcpChannelStats stats{};
    stats.idleReclaimed = reclaimedCount_;
    for (const auto& kv : sessions_)
    {
        const SessionState& st = kv.second;
        if (st.kcp != nullptr)
        {
            stats.sessionCount++;
            stats.crcOk += st.crcOk;
            stats.crcFail += st.crcFail;
        }
    }
    return stats;
}

std::vector<std::uint32_t> KcpChannel::ActiveSessionIds() const
{
    std::vector<std::uint32_t> ids;
    ids.reserve(sessions_.size());
    for (const auto& kv : sessions_)
    {
        if (kv.second.kcp != nullptr)
        {
            ids.push_back(kv.first);
        }
    }
    return ids;
}

void KcpChannel::ProcessIncoming()
{
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(socketHandle_);
    sockaddr_in remoteAddr{};
    int remoteLen = sizeof(remoteAddr);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(settings_.mtu + 24), 0);

    while (true)
    {
        const int bytes = ::recvfrom(sock,
                                     reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()),
                                     0,
                                     reinterpret_cast<SOCKADDR*>(&remoteAddr),
                                     &remoteLen);
        if (bytes > 0)
        {
            buffer.resize(static_cast<std::size_t>(bytes));
            wchar_t remoteText[64] = {0};
            InetNtopW(AF_INET, &remoteAddr.sin_addr, remoteText, 64);
            PeerEndpoint sender{};
            sender.host = remoteText;
            sender.port = ntohs(remoteAddr.sin_port);
            HandleDatagram(buffer, sender);
            buffer.assign(static_cast<std::size_t>(settings_.mtu + 24), 0);
        }
        else
        {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK)
            {
                std::wcerr << L"[kcp] recvfrom 错误: " << ToWideMessage(err) << L"\n";
            }
            break;
        }
    }
#endif
}

void KcpChannel::HandleDatagram(const std::vector<std::uint8_t>& buffer, const PeerEndpoint& sender)
{
    std::vector<std::uint8_t> payload = buffer;
    if (settings_.enableCrc32)
    {
        if (buffer.size() < sizeof(UdpFrame) || buffer.size() > settings_.maxFrameSize + sizeof(UdpFrame))
        {
            return;
        }
        if (!ValidateFrame(buffer, settings_.crcDropLog))
        {
            SessionState& st = EnsureSession(0, sender);
            st.crcFail++;
            return;
        }
        SessionState& st = EnsureSession(0, sender);
        st.crcOk++;
        payload.assign(buffer.begin() + static_cast<long long>(sizeof(UdpFrame)), buffer.end());
    }

    if (payload.size() < sizeof(std::uint32_t))
    {
        return;
    }

    std::uint32_t conv = 0;
    std::memcpy(&conv, payload.data(), sizeof(std::uint32_t));
    SessionState& state = EnsureSession(conv, sender);
    if (state.kcp == nullptr)
    {
        return;
    }

    const std::uint32_t now = NowMs();
    UpdatePeer(conv, state, sender, now);
    state.lastActiveMs = now;
    const int ret = ikcp_input(state.kcp, reinterpret_cast<const char*>(payload.data()), static_cast<long>(payload.size()));
    if (ret < 0)
    {
        std::wcerr << L"[kcp] ikcp_input 失败: " << ret << L"\n";
    }
}

void KcpChannel::UpdateSessions()
{
    const std::uint32_t now = NowMs();
    for (auto& kv : sessions_)
    {
        SessionState& state = kv.second;
        if (state.kcp == nullptr)
        {
            continue;
        }
        ikcp_update(state.kcp, now);

        char buffer[1500] = {0};
        int hr = ikcp_recv(state.kcp, buffer, sizeof(buffer));
        while (hr > 0)
        {
            ReceivedDatagram pkt{};
            pkt.payload.assign(buffer, buffer + hr);
            pkt.sender = state.peer;
            pkt.sessionId = kv.first;
            received_.push_back(pkt);
            state.lastActiveMs = now;
            hr = ikcp_recv(state.kcp, buffer, sizeof(buffer));
        }
    }
    CleanupStaleSessions(now);
}

SessionState& KcpChannel::EnsureSession(std::uint32_t sessionId, const PeerEndpoint& peer)
{
    const auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
    {
        return it->second;
    }

    SessionState state{};
    state.peer = peer;
    const std::uint32_t now = NowMs();
    state.lastActiveMs = now;
    state.lastSendMs = now;
    ikcpcb* kcp = ikcp_create(sessionId, this);
    if (kcp == nullptr)
    {
        sessions_[sessionId] = state;
        return sessions_[sessionId];
    }

    ikcp_setoutput(kcp, &KcpChannel::KcpOutput);
    ikcp_nodelay(kcp, settings_.noDelay ? 1 : 0, settings_.intervalMs, 2, 1);
    ikcp_wndsize(kcp, settings_.sendWindow, settings_.receiveWindow);
    ikcp_setmtu(kcp, settings_.mtu);
    state.kcp = kcp;
    if (!state.peer.host.empty() && state.peer.port != 0)
    {
        peerToSession_[BuildPeerKey(state.peer)] = sessionId;
    }
    sessions_[sessionId] = state;
    return sessions_[sessionId];
}

bool KcpChannel::SendRaw(const PeerEndpoint& peer, const std::vector<std::uint8_t>& frame)
{
#ifdef _WIN32
    SOCKET sock = static_cast<SOCKET>(socketHandle_);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer.port);
    if (::InetPtonW(AF_INET, peer.host.c_str(), &addr.sin_addr) != 1)
    {
        std::wcerr << L"[kcp] Send 解析地址失败: " << peer.host << L"\n";
        return false;
    }

    const int sent = ::sendto(sock,
                              reinterpret_cast<const char*>(frame.data()),
                              static_cast<int>(frame.size()),
                              0,
                              reinterpret_cast<SOCKADDR*>(&addr),
                              sizeof(addr));
    if (sent == SOCKET_ERROR)
    {
        std::wcerr << L"[kcp] sendto 失败: " << ToWideMessage(WSAGetLastError()) << L"\n";
        return false;
    }
    return true;
#else
    return false;
#endif
}

std::wstring KcpChannel::BuildPeerKey(const PeerEndpoint& peer) const
{
    return peer.host + L":" + std::to_wstring(peer.port);
}

void KcpChannel::Reset()
{
#ifdef _WIN32
    socketHandle_ = 0;
    if (winsockReady_)
    {
        WSACleanup();
        winsockReady_ = false;
    }
#endif
    DisposeSessions();
    boundPort_ = 0;
    lastReceived_.clear();
    lastSender_ = PeerEndpoint{};
    received_.clear();
    sessions_.clear();
    peerToSession_.clear();
    reclaimedCount_ = 0;
}

void KcpChannel::CleanupStaleSessions(std::uint32_t now)
{
    if (settings_.idleTimeoutMs == 0)
    {
        return;
    }

    std::vector<std::uint32_t> expired;
    expired.reserve(sessions_.size());
    for (const auto& kv : sessions_)
    {
        const SessionState& state = kv.second;
        if (state.lastActiveMs != 0 && now > state.lastActiveMs + settings_.idleTimeoutMs)
        {
            expired.push_back(kv.first);
        }
    }

    for (std::uint32_t id : expired)
    {
        auto it = sessions_.find(id);
        if (it == sessions_.end())
        {
            continue;
        }
        if (it->second.kcp != nullptr)
        {
            ikcp_release(it->second.kcp);
        }
        const std::wstring peerKey = BuildPeerKey(it->second.peer);
        peerToSession_.erase(peerKey);
        sessions_.erase(it);
        reclaimedCount_++;
        std::wcout << L"[kcp] 会话 " << id << L" 已超时回收\n";
    }
}

void KcpChannel::UpdatePeer(std::uint32_t sessionId, SessionState& state, const PeerEndpoint& peer, std::uint32_t now)
{
    if (peer.host.empty() || peer.port == 0)
    {
        state.lastActiveMs = now;
        return;
    }

    const bool samePeer = (state.peer.host == peer.host && state.peer.port == peer.port);
    if (!samePeer)
    {
        const bool allowRebind = settings_.peerRebindCooldownMs == 0 ||
                                 state.lastActiveMs == 0 || now >= state.lastActiveMs + settings_.peerRebindCooldownMs;
        if (allowRebind)
        {
            const std::wstring oldKey = BuildPeerKey(state.peer);
            if (!oldKey.empty())
            {
                peerToSession_.erase(oldKey);
            }
            state.peer = peer;
            std::wcout << L"[kcp] 会话 " << sessionId << L" 端点更新为 " << peer.host << L":" << peer.port << L"\n";
        }
    }

    peerToSession_[BuildPeerKey(state.peer)] = sessionId;
    state.lastActiveMs = now;
}

int KcpChannel::KcpOutput(const char* buf, int len, ikcpcb* kcp, void* user)
{
    if (user == nullptr || buf == nullptr || len <= 0)
    {
        return -1;
    }

    KcpChannel* channel = reinterpret_cast<KcpChannel*>(user);
    const auto it = channel->sessions_.find(kcp->conv);
    if (it == channel->sessions_.end())
    {
        return -2;
    }
    const PeerEndpoint& peer = it->second.peer;
    std::vector<std::uint8_t> frame(buf, buf + len);
    if (channel->settings_.enableCrc32 && frame.size() <= channel->settings_.maxFrameSize)
    {
        frame = WrapFrame(frame, kcp->conv);
    }
    return channel->SendRaw(peer, frame) ? 0 : -3;
}

std::uint32_t KcpChannel::NowMs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void KcpChannel::DisposeSessions()
{
    for (auto& kv : sessions_)
    {
        if (kv.second.kcp != nullptr)
        {
            ikcp_release(kv.second.kcp);
            kv.second.kcp = nullptr;
        }
    }
}
}  // namespace mi::shared::net
